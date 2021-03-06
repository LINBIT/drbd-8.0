#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
# error "use a 2.6 kernel, please"
#endif

/* for the proc_create wrapper */
#include <linux/proc_fs.h>

/* struct page has a union in 2.6.15 ...
 * an anonymous union and struct since 2.6.16
 * or in fc5 "2.6.15" */
#include <linux/mm.h>
#ifndef page_private
# define page_private(page)		((page)->private)
# define set_page_private(page, v)	((page)->private = (v))
#endif

/* mutex was not available before 2.6.16.
 * various vendors provide various degrees of backports.
 * we provide the missing parts ourselves, if neccessary.
 * this one is for RHEL/Centos 4 */
#if defined(mutex_lock) && !defined(mutex_is_locked)
#define mutex_is_locked(m) (atomic_read(&(m)->count) != 1)
#endif

/* see get_sb_bdev and bd_claim */
extern char *drbd_sec_holder;

static inline sector_t drbd_get_hardsect(struct block_device *bdev)
{
	return bdev->bd_disk->queue->hardsect_size;
}

/* Returns the number of 512 byte sectors of the device */
static inline sector_t drbd_get_capacity(struct block_device *bdev)
{
	/* return bdev ? get_capacity(bdev->bd_disk) : 0; */
	return bdev ? bdev->bd_inode->i_size >> 9 : 0;
}

/* sets the number of 512 byte sectors of our virtual device */
static inline void drbd_set_my_capacity(struct drbd_conf *mdev,
					sector_t size)
{
	/* set_capacity(mdev->this_bdev->bd_disk, size); */
	set_capacity(mdev->vdisk, size);
	mdev->this_bdev->bd_inode->i_size = (loff_t)size << 9;
}

#define drbd_bio_uptodate(bio) bio_flagged(bio, BIO_UPTODATE)

static inline int drbd_bio_has_active_page(struct bio *bio)
{
	struct bio_vec *bvec;
	int i;

	__bio_for_each_segment(bvec, bio, i, 0) {
		if (page_count(bvec->bv_page) > 1)
			return 1;
	}

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
/* Before Linux-2.6.24 bie_endio() had the size of the bio as second argument.
   See 6712ecf8f648118c3363c142196418f89a510b90 */
#define bio_endio(B,E) bio_endio(B, (B)->bi_size, E)
#define BIO_ENDIO_TYPE int
#define BIO_ENDIO_ARGS(b,e) (b, unsigned int bytes_done, e)
#define BIO_ENDIO_FN_START if (bio->bi_size) return 1
#define BIO_ENDIO_FN_RETURN return 0
#else
#define BIO_ENDIO_TYPE void
#define BIO_ENDIO_ARGS(b,e) (b,e)
#define BIO_ENDIO_FN_START do {} while (0)
#define BIO_ENDIO_FN_RETURN return
#endif

/* bi_end_io handlers */
extern BIO_ENDIO_TYPE drbd_md_io_complete BIO_ENDIO_ARGS(struct bio *bio, int error);
extern BIO_ENDIO_TYPE drbd_endio_read_sec BIO_ENDIO_ARGS(struct bio *bio, int error);
extern BIO_ENDIO_TYPE drbd_endio_write_sec BIO_ENDIO_ARGS(struct bio *bio, int error);
extern BIO_ENDIO_TYPE drbd_endio_pri BIO_ENDIO_ARGS(struct bio *bio, int error);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
/* Before 2.6.23 (with 20c2df83d25c6a95affe6157a4c9cac4cf5ffaac) kmem_cache_create had a
   ctor and a dtor */
#define kmem_cache_create(N,S,A,F,C) kmem_cache_create(N,S,A,F,C,NULL)
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,26)
# undef HAVE_bvec_merge_data
# define HAVE_bvec_merge_data 1
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
static inline void sg_set_page(struct scatterlist *sg, struct page *page,
			       unsigned int len, unsigned int offset)
{
	sg->page   = page;
	sg->offset = offset;
	sg->length = len;
}

#define sg_init_table(S,N) ({})

#ifdef NEED_SG_SET_BUF
static inline void sg_set_buf(struct scatterlist *sg, const void *buf,
			      unsigned int buflen)
{
	sg_set_page(sg, virt_to_page(buf), buflen, offset_in_page(buf));
}
#endif

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28)
# define BD_OPS_USE_FMODE
#endif

/*
 * used to submit our private bio
 */
static inline void drbd_generic_make_request(struct drbd_conf *mdev,
					     int fault_type, struct bio *bio)
{
	__release(local);
	if (!bio->bi_bdev) {
		printk(KERN_ERR "drbd%d: drbd_generic_make_request: "
				"bio->bi_bdev == NULL\n",
		       mdev_to_minor(mdev));
		dump_stack();
		bio_endio(bio, -ENODEV);
		return;
	}

	if (FAULT_ACTIVE(mdev, fault_type))
		bio_endio(bio, -EIO);
	else
		generic_make_request(bio);
}

static inline void drbd_plug_device(struct drbd_conf *mdev)
{
	struct request_queue *q;
	q = bdev_get_queue(mdev->this_bdev);

	spin_lock_irq(q->queue_lock);

/* XXX the check on !blk_queue_plugged is redundant,
 * implicitly checked in blk_plug_device */

	if (!blk_queue_plugged(q)) {
		blk_plug_device(q);
		del_timer(&q->unplug_timer);
		/* unplugging should not happen automatically... */
	}
	spin_unlock_irq(q->queue_lock);
}

#ifdef DEFINE_SOCK_CREATE_KERN
#define sock_create_kern sock_create
#endif

#ifdef USE_KMEM_CACHE_S
#define kmem_cache kmem_cache_s
#endif

#ifdef DEFINE_KERNEL_SOCK_SHUTDOWN
enum sock_shutdown_cmd {
	SHUT_RD = 0,
	SHUT_WR = 1,
	SHUT_RDWR = 2,
};
static inline int kernel_sock_shutdown(struct socket *sock, enum sock_shutdown_cmd how)
{
	return sock->ops->shutdown(sock, how);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
static inline void drbd_unregister_blkdev(unsigned int major, const char *name)
{
	int ret = unregister_blkdev(major, name);
	if (ret)
		printk(KERN_ERR "drbd: unregister of device failed\n");
}
#else
#define drbd_unregister_blkdev unregister_blkdev
#endif

#ifdef NEED_BACKPORT_OF_ATOMIC_ADD

#if defined(__x86_64__)

static __inline__ int atomic_add_return(int i, atomic_t *v)
{
	int __i = i;
	__asm__ __volatile__(
		LOCK_PREFIX "xaddl %0, %1;"
		:"=r"(i)
		:"m"(v->counter), "0"(i));
	return i + __i;
}

static __inline__ int atomic_sub_return(int i, atomic_t *v)
{
	return atomic_add_return(-i, v);
}

#define atomic_inc_return(v)  (atomic_add_return(1,v))
#define atomic_dec_return(v)  (atomic_sub_return(1,v))

#elif defined(__i386__) || defined(__arch_um__)

static __inline__ int atomic_add_return(int i, atomic_t *v)
{
	int __i;
#ifdef CONFIG_M386
	unsigned long flags;
	if(unlikely(boot_cpu_data.x86==3))
		goto no_xadd;
#endif
	/* Modern 486+ processor */
	__i = i;
	__asm__ __volatile__(
		LOCK_PREFIX "xaddl %0, %1;"
		:"=r"(i)
		:"m"(v->counter), "0"(i));
	return i + __i;

#ifdef CONFIG_M386
no_xadd: /* Legacy 386 processor */
	local_irq_save(flags);
	__i = atomic_read(v);
	atomic_set(v, i + __i);
	local_irq_restore(flags);
	return i + __i;
#endif
}

static __inline__ int atomic_sub_return(int i, atomic_t *v)
{
	return atomic_add_return(-i, v);
}

#define atomic_inc_return(v)  (atomic_add_return(1,v))
#define atomic_dec_return(v)  (atomic_sub_return(1,v))

#else
# error "You need to copy/past atomic_inc_return()/atomic_dec_return() here"
# error "for your architecture. (Hint: Kernels after 2.6.10 have those"
# error "by default! Using a later kernel might be less effort!)"
#endif

#endif

#if !defined(CRYPTO_ALG_ASYNC)
/* With Linux-2.6.19 the crypto API changed! */
/* This is not a generic backport of the new api, it just implements
   the corner case of "hmac(xxx)".  */

#define CRYPTO_ALG_ASYNC 4711
#define CRYPTO_ALG_TYPE_HASH CRYPTO_ALG_TYPE_DIGEST

struct crypto_hash {
	struct crypto_tfm *base;
	const u8 *key;
	int keylen;
};

struct hash_desc {
	struct crypto_hash *tfm;
	u32 flags;
};

static inline struct crypto_hash *
crypto_alloc_hash(char *alg_name, u32 type, u32 mask)
{
	struct crypto_hash *ch;
	char *closing_bracket;

	/* "hmac(xxx)" is in alg_name we need that xxx. */
	closing_bracket = strchr(alg_name, ')');
	if (!closing_bracket)
		return ERR_PTR(-ENOENT);
	if (closing_bracket-alg_name < 6)
		return ERR_PTR(-ENOENT);

	ch = kmalloc(sizeof(struct crypto_hash), GFP_KERNEL);
	if (!ch)
		return ERR_PTR(-ENOMEM);

	*closing_bracket = 0;
	ch->base = crypto_alloc_tfm(alg_name + 5, 0);
	*closing_bracket = ')';

	if (ch->base == NULL) {
		kfree(ch);
		return ERR_PTR(-ENOMEM);
	}

	return ch;
}

static inline int
crypto_hash_setkey(struct crypto_hash *hash, const u8 *key, unsigned int keylen)
{
	hash->key = key;
	hash->keylen = keylen;

	return 0;
}

static inline int
crypto_hash_digest(struct hash_desc *desc, struct scatterlist *sg,
		   unsigned int nbytes, u8 *out)
{

	crypto_hmac(desc->tfm->base, (u8 *)desc->tfm->key,
		    &desc->tfm->keylen, sg, 1 /* ! */ , out);
	/* ! this is not generic. Would need to convert nbytes -> nsg */

	return 0;
}

static inline void crypto_free_hash(struct crypto_hash *tfm)
{
	if (!tfm)
		return;
	crypto_free_tfm(tfm->base);
	kfree(tfm);
}

static inline unsigned int crypto_hash_digestsize(struct crypto_hash *tfm)
{
	return crypto_tfm_alg_digestsize(tfm->base);
}

static inline struct crypto_tfm *crypto_hash_tfm(struct crypto_hash *tfm)
{
	return tfm->base;
}

#endif

#ifdef NEED_BACKPORT_OF_KZALLOC
static inline void *kzalloc(size_t size, int flags)
{
	void *rv = kmalloc(size, flags);
	if (rv)
		memset(rv, 0, size);

	return rv;
}
#endif

#ifndef __CHECKER__
# undef __cond_lock
# define __cond_lock(x,c) (c)
#endif

#ifndef KERNEL_HAS_GFP_T
#define KERNEL_HAS_GFP_T
typedef unsigned gfp_t;
#endif


/* struct kvec didn't exist before 2.6.8, this is an ugly
 * #define to work around it ... - jt */

#ifndef KERNEL_HAS_KVEC
#define kvec iovec
#endif


/* see upstream commits
 * 2d3a4e3666325a9709cc8ea2e88151394e8f20fc (in 2.6.25-rc1)
 * 59b7435149eab2dd06dd678742faff6049cb655f (in 2.6.26-rc1)
 * this "backport" does not close the race that lead to the API change,
 * but only provides an equivalent function call.
 */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
/* maybe someone will backport this upstream somewhen. */
#define KERNEL_HAS_PROC_CREATE
#endif
#ifndef KERNEL_HAS_PROC_CREATE
static inline struct proc_dir_entry *proc_create(const char *name,
	mode_t mode, struct proc_dir_entry *parent,
	struct file_operations *proc_fops)
{
	struct proc_dir_entry *pde = create_proc_entry(name, mode, parent);
	if (pde)
		pde->proc_fops = proc_fops;
	return pde;
}
#endif
