/*
 *	SUCS NET3:
 *
 *	Generic datagram handling routines. These are generic for all
 *	protocols. Possibly a generic IP version on top of these would
 *	make sense. Not tonight however 8-).
 *	This is used because UDP, RAW, PACKET, DDP, IPX, AX.25 and
 *	NetROM layer all have identical poll code and mostly
 *	identical recvmsg() code. So we share it here. The poll was
 *	shared before but buried in udp.c so I moved it.
 *
 *	Authors:	Alan Cox <alan@lxorguk.ukuu.org.uk>. (datagram_poll() from old
 *						     udp.c code)
 *
 *	Fixes:
 *		Alan Cox	:	NULL return from skb_peek_copy()
 *					understood
 *		Alan Cox	:	Rewrote skb_read_datagram to avoid the
 *					skb_peek_copy stuff.
 *		Alan Cox	:	Added support for SOCK_SEQPACKET.
 *					IPX can no longer use the SO_TYPE hack
 *					but AX.25 now works right, and SPX is
 *					feasible.
 *		Alan Cox	:	Fixed write poll of non IP protocol
 *					crash.
 *		Florian  La Roche:	Changed for my new skbuff handling.
 *		Darryl Miles	:	Fixed non-blocking SOCK_SEQPACKET.
 *		Linus Torvalds	:	BSD semantic fixes.
 *		Alan Cox	:	Datagram iovec handling
 *		Darryl Miles	:	Fixed non-blocking SOCK_STREAM.
 *		Alan Cox	:	POSIXisms
 *		Pete Wyckoff    :       Unconnected accept() fix.
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/poll.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include <net/protocol.h>
#include <linux/skbuff.h>

#include <net/checksum.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <trace/events/skb.h>

/*
 *	Is a socket 'connection oriented' ?
 */
static inline int connection_based(struct sock *sk)
{
	return sk->sk_type == SOCK_SEQPACKET || sk->sk_type == SOCK_STREAM;
}

static int receiver_wake_function(wait_queue_t *wait, unsigned mode, int sync,
				  void *key)
{
	unsigned long bits = (unsigned long)key;

	/*
	 * Avoid a wakeup if event not interesting for us
	 */
	if (bits && !(bits & (POLLIN | POLLERR)))
		return 0;
	return autoremove_wake_function(wait, mode, sync, key);
}
/*
 * Wait for a packet..
 */
static int wait_for_packet(struct sock *sk, int *err, long *timeo_p)
{
	int error;
	DEFINE_WAIT_FUNC(wait, receiver_wake_function);
    /* 
     前面的操作都是初始化wait，为将socket加入wait队列作准备，这部分代码牵涉到进程调度。关于进程调度，我      只是知道一些皮毛，留在以后学习。这里只需要将其看作是一些加入wait队列的准备工作即可，并不影响理解代码      。
     */
	prepare_to_wait_exclusive(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);

	/* Socket errors? */
	error = sock_error(sk);
	if (error)
		goto out_err;
    /* 一个完备检测。在决定wait和调用wait之间，有数据包到了，那么就不需要wait，所以这里再次检查socket      的队列是否为空 */

	if (!skb_queue_empty(&sk->sk_receive_queue))
		goto out;

	/* Socket shut down? */
	/* 完备检测。也许socket无数据包读取，因为socket已经被另外的线程关闭了。这样可以保证关闭socket的时      候，不会导致其他的socket的读写操作被阻塞。*/
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		goto out_noerr;

	/* Sequenced packets can come disconnected.
	 * If so we report the problem
	 *//* 对于面向连接的socket进行检查。如果是面向连接的socket，如果不是已经建立连接或者正在监听状态的so       cket是不可能有数据包的。不然即出错*/
	error = -ENOTCONN;
	if (connection_based(sk) &&
	    !(sk->sk_state == TCP_ESTABLISHED || sk->sk_state == TCP_LISTEN))
		goto out_err;

	/* handle signals */
	if (signal_pending(current))/* 检查是否有pending的signal，保证阻塞时，进程可以被signal唤醒 */
		goto interrupted;

	error = 0;
	*timeo_p = schedule_timeout(*timeo_p); /* sleep本进程，直至满足唤醒条件或者被信号唤醒――因为前面设置了TASK_INTERRUPTIBLE*/
out:
	finish_wait(sk_sleep(sk), &wait); /* wait队列的清理工作 */
	return error;
interrupted:
	error = sock_intr_errno(*timeo_p);
out_err:
	*err = error;
	goto out;
out_noerr:
	*err = 0;
	error = 1;
	goto out;
}

/**
 *	__skb_recv_datagram - Receive a datagram skbuff
 *	@sk: socket
 *	@flags: MSG_ flags
 *	@peeked: returns non-zero if this packet has been seen before
 *	@err: error code returned
 *
 *	Get a datagram skbuff, understands the peeking, nonblocking wakeups
 *	and possible races. This replaces identical code in packet, raw and
 *	udp, as well as the IPX AX.25 and Appletalk. It also finally fixes
 *	the long standing peek and read race for datagram sockets. If you
 *	alter this routine remember it must be re-entrant.
 *
 *	This function will lock the socket if a skb is returned, so the caller
 *	needs to unlock the socket in that case (usually by calling
 *	skb_free_datagram)
 *
 *	* It does not lock socket since today. This function is
 *	* free of race conditions. This measure should/can improve
 *	* significantly datagram socket latencies at high loads,
 *	* when data copying to user space takes lots of time.
 *	* (BTW I've just killed the last cli() in IP/IPv6/core/netlink/packet
 *	*  8) Great win.)
 *	*			                    --ANK (980729)
 *
 *	The order of the tests when we find no data waiting are specified
 *	quite explicitly by POSIX 1003.1g, don't change them without having
 *	the standard around please.
 *///从sk->sk_receive_queue队列中取出SKB
struct sk_buff *__skb_recv_datagram(struct sock *sk, unsigned flags,
				    int *peeked, int *err)
{
	struct sk_buff *skb;
	long timeo;
	/*
	 * Caller is allowed not to check sk->sk_err before skb_recv_datagram()
	 */
	int error = sock_error(sk);

	if (error)
		goto no_packet;

    // /* 当socket为阻塞时，获取timeout的值 */
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	do {
		/* Again only user level code calls this function, so nothing
		 * interrupt level will suddenly eat the receive_queue.
		 *
		 * Look at current nfs client by the way...
		 * However, this function was corrent in any case. 8)
		 */
		unsigned long cpu_flags;
         /* 
         当查看socket是否有数据包时，需要上锁，因为需要保证其它线程不会将数据包取走。
         */
		spin_lock_irqsave(&sk->sk_receive_queue.lock, cpu_flags);
		skb = skb_peek(&sk->sk_receive_queue); /* 查看在socket的buffer中是否有数据包 */
		if (skb) {
			*peeked = skb->peeked;
			if (flags & MSG_PEEK) {
			     /* 
                设置MSG_PEEK，表示用户不是真的要读取数据，只是一个peek调用。
                那么并不真正读取数据
                */
				skb->peeked = 1;
				atomic_inc(&skb->users);
			} else
				__skb_unlink(skb, &sk->sk_receive_queue);//从队列中取出数据，即可看作读出数据
		}
		spin_unlock_irqrestore(&sk->sk_receive_queue.lock, cpu_flags);

		if (skb) // 有数据包，返回skb
			return skb;

         /*
        timeo为0，有2中情况：1种是socket为非阻塞的，第2种，即socket阻塞的时间已经超过了timeo的值，
	那么就跳到no_packet处理 
        */
		/* User doesn't want to wait */
		error = -EAGAIN;
		if (!timeo)
			goto no_packet;

	} while (!wait_for_packet(sk, err, &timeo));//阻塞进程，等待数据包

	return NULL;

no_packet:
	*err = error;
	return NULL;
}
EXPORT_SYMBOL(__skb_recv_datagram);
//从等待队列中接受一个数据包
struct sk_buff *skb_recv_datagram(struct sock *sk, unsigned flags,
				  int noblock, int *err)
{
	int peeked;

	return __skb_recv_datagram(sk, flags | (noblock ? MSG_DONTWAIT : 0),
				   &peeked, err);
}

void skb_free_datagram(struct sock *sk, struct sk_buff *skb)
{
	consume_skb(skb);
	sk_mem_reclaim_partial(sk);
}
EXPORT_SYMBOL(skb_free_datagram);

void skb_free_datagram_locked(struct sock *sk, struct sk_buff *skb)
{
	bool slow;

	if (likely(atomic_read(&skb->users) == 1))
		smp_rmb();
	else if (likely(!atomic_dec_and_test(&skb->users)))
		return;

	slow = lock_sock_fast(sk);
	skb_orphan(skb);
	sk_mem_reclaim_partial(sk);
	unlock_sock_fast(sk, slow);

	/* skb is now orphaned, can be freed outside of locked section */
	__kfree_skb(skb);
}
EXPORT_SYMBOL(skb_free_datagram_locked);

/**
 *	skb_kill_datagram - Free a datagram skbuff forcibly
 *	@sk: socket
 *	@skb: datagram skbuff
 *	@flags: MSG_ flags
 *
 *	This function frees a datagram skbuff that was received by
 *	skb_recv_datagram.  The flags argument must match the one
 *	used for skb_recv_datagram.
 *
 *	If the MSG_PEEK flag is set, and the packet is still on the
 *	receive queue of the socket, it will be taken off the queue
 *	before it is freed.
 *
 *	This function currently only disables BH when acquiring the
 *	sk_receive_queue lock.  Therefore it must not be used in a
 *	context where that lock is acquired in an IRQ context.
 *
 *	It returns 0 if the packet was removed by us.
 */

int skb_kill_datagram(struct sock *sk, struct sk_buff *skb, unsigned int flags)
{
	int err = 0;

	if (flags & MSG_PEEK) {
		err = -ENOENT;
		spin_lock_bh(&sk->sk_receive_queue.lock);
		if (skb == skb_peek(&sk->sk_receive_queue)) {
			__skb_unlink(skb, &sk->sk_receive_queue);
			atomic_dec(&skb->users);
			err = 0;
		}
		spin_unlock_bh(&sk->sk_receive_queue.lock);
	}

	kfree_skb(skb);
	atomic_inc(&sk->sk_drops);
	sk_mem_reclaim_partial(sk);

	return err;
}

EXPORT_SYMBOL(skb_kill_datagram);

/**
 *	skb_copy_datagram_iovec - Copy a datagram to an iovec.
 *	@skb: buffer to copy
 *	@offset: offset in the buffer to start copying from
 *	@to: io vector to copy to
 *	@len: amount of data to copy from buffer to iovec
 *
 *	Note: the iovec is modified during the copy.
 *///将skb拷贝至msghdr的iovec中
int skb_copy_datagram_iovec(const struct sk_buff *skb, int offset,
			    struct iovec *to, int len)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;

	trace_skb_copy_datagram_iovec(skb, len);

	/* Copy header. */
	if (copy > 0) {
		if (copy > len)
			copy = len;
		if (memcpy_toiovec(to, skb->data + offset, copy))
			goto fault;
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
	}

	/* Copy paged appendix. Hmm... why does this look so complicated? */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_shinfo(skb)->frags[i].size;
		if ((copy = end - offset) > 0) {
			int err;
			u8  *vaddr;
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct page *page = frag->page;

			if (copy > len)
				copy = len;
			vaddr = kmap(page);
			err = memcpy_toiovec(to, vaddr + frag->page_offset +
					     offset - start, copy);
			kunmap(page);
			if (err)
				goto fault;
			if (!(len -= copy))
				return 0;
			offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			if (skb_copy_datagram_iovec(frag_iter,
						    offset - start,
						    to, copy))
				goto fault;
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
		}
		start = end;
	}
	if (!len)
		return 0;

fault:
	return -EFAULT;
}

/**
 *	skb_copy_datagram_const_iovec - Copy a datagram to an iovec.
 *	@skb: buffer to copy
 *	@offset: offset in the buffer to start copying from
 *	@to: io vector to copy to
 *	@to_offset: offset in the io vector to start copying to
 *	@len: amount of data to copy from buffer to iovec
 *
 *	Returns 0 or -EFAULT.
 *	Note: the iovec is not modified during the copy.
 */
int skb_copy_datagram_const_iovec(const struct sk_buff *skb, int offset,
				  const struct iovec *to, int to_offset,
				  int len)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;

	/* Copy header. */
	if (copy > 0) {
		if (copy > len)
			copy = len;
		if (memcpy_toiovecend(to, skb->data + offset, to_offset, copy))
			goto fault;
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
		to_offset += copy;
	}

	/* Copy paged appendix. Hmm... why does this look so complicated? */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_shinfo(skb)->frags[i].size;
		if ((copy = end - offset) > 0) {
			int err;
			u8  *vaddr;
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct page *page = frag->page;

			if (copy > len)
				copy = len;
			vaddr = kmap(page);
			err = memcpy_toiovecend(to, vaddr + frag->page_offset +
						offset - start, to_offset, copy);
			kunmap(page);
			if (err)
				goto fault;
			if (!(len -= copy))
				return 0;
			offset += copy;
			to_offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			if (skb_copy_datagram_const_iovec(frag_iter,
							  offset - start,
							  to, to_offset,
							  copy))
				goto fault;
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			to_offset += copy;
		}
		start = end;
	}
	if (!len)
		return 0;

fault:
	return -EFAULT;
}
EXPORT_SYMBOL(skb_copy_datagram_const_iovec);

/**
 *	skb_copy_datagram_from_iovec - Copy a datagram from an iovec.
 *	@skb: buffer to copy
 *	@offset: offset in the buffer to start copying to
 *	@from: io vector to copy to
 *	@from_offset: offset in the io vector to start copying from
 *	@len: amount of data to copy to buffer from iovec
 *
 *	Returns 0 or -EFAULT.
 *	Note: the iovec is not modified during the copy.
 */
int skb_copy_datagram_from_iovec(struct sk_buff *skb, int offset,
				 const struct iovec *from, int from_offset,
				 int len)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;

	/* Copy header. */
	if (copy > 0) {
		if (copy > len)
			copy = len;
		if (memcpy_fromiovecend(skb->data + offset, from, from_offset,
					copy))
			goto fault;
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
		from_offset += copy;
	}

	/* Copy paged appendix. Hmm... why does this look so complicated? */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_shinfo(skb)->frags[i].size;
		if ((copy = end - offset) > 0) {
			int err;
			u8  *vaddr;
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct page *page = frag->page;

			if (copy > len)
				copy = len;
			vaddr = kmap(page);
			err = memcpy_fromiovecend(vaddr + frag->page_offset +
						  offset - start,
						  from, from_offset, copy);
			kunmap(page);
			if (err)
				goto fault;

			if (!(len -= copy))
				return 0;
			offset += copy;
			from_offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			if (skb_copy_datagram_from_iovec(frag_iter,
							 offset - start,
							 from,
							 from_offset,
							 copy))
				goto fault;
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			from_offset += copy;
		}
		start = end;
	}
	if (!len)
		return 0;

fault:
	return -EFAULT;
}
EXPORT_SYMBOL(skb_copy_datagram_from_iovec);

static int skb_copy_and_csum_datagram(const struct sk_buff *skb, int offset,
				      u8 __user *to, int len,
				      __wsum *csump)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;
	int pos = 0;

	/* Copy header. */
	if (copy > 0) {
		int err = 0;
		if (copy > len)
			copy = len;
		*csump = csum_and_copy_to_user(skb->data + offset, to, copy,
					       *csump, &err);
		if (err)
			goto fault;
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
		to += copy;
		pos = copy;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_shinfo(skb)->frags[i].size;
		if ((copy = end - offset) > 0) {
			__wsum csum2;
			int err = 0;
			u8  *vaddr;
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct page *page = frag->page;

			if (copy > len)
				copy = len;
			vaddr = kmap(page);
			csum2 = csum_and_copy_to_user(vaddr +
							frag->page_offset +
							offset - start,
						      to, copy, 0, &err);
			kunmap(page);
			if (err)
				goto fault;
			*csump = csum_block_add(*csump, csum2, pos);
			if (!(len -= copy))
				return 0;
			offset += copy;
			to += copy;
			pos += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			__wsum csum2 = 0;
			if (copy > len)
				copy = len;
			if (skb_copy_and_csum_datagram(frag_iter,
						       offset - start,
						       to, copy,
						       &csum2))
				goto fault;
			*csump = csum_block_add(*csump, csum2, pos);
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			to += copy;
			pos += copy;
		}
		start = end;
	}
	if (!len)
		return 0;

fault:
	return -EFAULT;
}

__sum16 __skb_checksum_complete_head(struct sk_buff *skb, int len)
{
	__sum16 sum;

	sum = csum_fold(skb_checksum(skb, 0, len, skb->csum));
	if (likely(!sum)) {
		if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE))
			netdev_rx_csum_fault(skb->dev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	}
	return sum;
}
EXPORT_SYMBOL(__skb_checksum_complete_head);

__sum16 __skb_checksum_complete(struct sk_buff *skb)
{
	return __skb_checksum_complete_head(skb, skb->len);
}
EXPORT_SYMBOL(__skb_checksum_complete);

/**
 *	skb_copy_and_csum_datagram_iovec - Copy and checkum skb to user iovec.
 *	@skb: skbuff
 *	@hlen: hardware length
 *	@iov: io vector
 *
 *	Caller _must_ check that skb will fit to this iovec.
 *
 *	Returns: 0       - success.
 *		 -EINVAL - checksum failure.
 *		 -EFAULT - fault during copy. Beware, in this case iovec
 *			   can be modified!
 */
int skb_copy_and_csum_datagram_iovec(struct sk_buff *skb,
				     int hlen, struct iovec *iov)
{
	__wsum csum;
	int chunk = skb->len - hlen;

	if (!chunk)
		return 0;

	/* Skip filled elements.
	 * Pretty silly, look at memcpy_toiovec, though 8)
	 */
	while (!iov->iov_len)
		iov++;

	if (iov->iov_len < chunk) {
		if (__skb_checksum_complete(skb))
			goto csum_error;
		if (skb_copy_datagram_iovec(skb, hlen, iov, chunk))
			goto fault;
	} else {
		csum = csum_partial(skb->data, hlen, skb->csum);
		if (skb_copy_and_csum_datagram(skb, hlen, iov->iov_base,
					       chunk, &csum))
			goto fault;
		if (csum_fold(csum))
			goto csum_error;
		if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE))
			netdev_rx_csum_fault(skb->dev);
		iov->iov_len -= chunk;
		iov->iov_base += chunk;
	}
	return 0;
csum_error:
	return -EINVAL;
fault:
	return -EFAULT;
}

/**
 * 	datagram_poll - generic datagram poll
 *	@file: file struct
 *	@sock: socket
 *	@wait: poll table
 *
 *	Datagram poll: Again totally generic. This also handles
 *	sequenced packet sockets providing the socket receive queue
 *	is only ever holding data ready to receive.
 *
 *	Note: when you _don't_ use this routine for this protocol,
 *	and you use a different write policy from sock_writeable()
 *	then please supply your own write_space callback.
 */
unsigned int datagram_poll(struct file *file, struct socket *sock,
			   poll_table *wait)
{
	struct sock *sk = sock->sk;
	unsigned int mask;

	sock_poll_wait(file, sk_sleep(sk), wait);
	mask = 0;

	/* exceptional events? */
	if (sk->sk_err || !skb_queue_empty(&sk->sk_error_queue))
		mask |= POLLERR;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLRDHUP;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= POLLHUP;

	/* readable? */
	if (!skb_queue_empty(&sk->sk_receive_queue) ||
	    (sk->sk_shutdown & RCV_SHUTDOWN))
		mask |= POLLIN | POLLRDNORM;

	/* Connection-based need to check for termination and startup */
	if (connection_based(sk)) {
		if (sk->sk_state == TCP_CLOSE)
			mask |= POLLHUP;
		/* connection hasn't started yet? */
		if (sk->sk_state == TCP_SYN_SENT)
			return mask;
	}

	/* writable? */
	if (sock_writeable(sk))
		mask |= POLLOUT | POLLWRNORM | POLLWRBAND;
	else
		set_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

	return mask;
}

EXPORT_SYMBOL(datagram_poll);
EXPORT_SYMBOL(skb_copy_and_csum_datagram_iovec);
EXPORT_SYMBOL(skb_copy_datagram_iovec);
EXPORT_SYMBOL(skb_recv_datagram);
