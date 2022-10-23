#ifndef BLOCKDEV_H__
#define BLOCKDEV_H__

#include <inttypes.h>
#include <stdio.h>
#include <pico/object.h>

typedef struct pico_blockdev__ pico_blockdev_t;

typedef struct
{
    uint32_t sector_size;
    uint32_t total_sectors;
} pico_blockdev_info_t;

typedef struct
{
    int (*init)(pico_blockdev_t *dev);
    int (*read_sector)(pico_blockdev_t *dev, unsigned char* data, uint32_t start_sector, unsigned count);
    int (*write_sector)(pico_blockdev_t *dev, const unsigned char* data, uint32_t start_sector, unsigned count);
    int (*ioctl)(pico_blockdev_t *dev, unsigned char cmd, void* data);
    void (*destroy)(pico_blockdev_t *dev);
} pico_blockdev_ops_t;

struct pico_blockdev_link_entry
{
    pico_blockdev_t *dev;
    struct pico_blockdev_link_entry *next;
};

struct pico_blockdev__
{
    pico_object_t obj;
    const pico_blockdev_ops_t *ops;
    struct pico_blockdev__ *parent;
    struct pico_blockdev_link_entry *children;
    /* Other dev-specific data below */
};


/*
 Supported IOCTLs
 */
#define PICO_IOCTL_BLKGETSIZE (0)  /* Get device size in sectors */
#define PICO_IOCTL_BLKSSZGET (1)   /* Get sector size in bytes */
#define PICO_IOCTL_BLKROGET (2)    /* Get readonly flag */
#define PICO_IOCTL_BLKFLSBUF (3)   /* Sync */
#define PICO_IOCTL_HDIO_GETGEO (4)

/* Returns number of sectors read */
int pico_blockdev_read_sector(pico_blockdev_t *dev, unsigned char* data, uint32_t start_sector, unsigned count);
/* Returns number of sectors written */
int pico_blockdev_write_sector(pico_blockdev_t *dev, const unsigned char* data, uint32_t start_sector, unsigned count);
int pico_blockdev_ioctl(pico_blockdev_t *dev, unsigned char cmd, void* data);
int pico_blockdev_init(pico_blockdev_t *dev, const pico_blockdev_ops_t *ops);
bool pico_blockdev_has_children(pico_blockdev_t *dev);

int pico_blockdev_register(pico_blockdev_t *dev);
void pico_blockdev_unregister(pico_blockdev_t *dev);

void pico_blockdev_register_event(pico_blockdev_t *dev);
void pico_blockdev_unregister_event(pico_blockdev_t *dev);

/*static inline void pico_blockdev_set_parent(pico_blockdev_t *child, pico_blockdev_t *parent)
{
    pico_object_ref(&parent->obj);
    child->parent = parent;
} */

int pico_blockdev_add_child(pico_blockdev_t *dev, pico_blockdev_t *child);

static inline pico_blockdev_t *pico_blockdev_ref(pico_blockdev_t *dev)
{
    return (pico_blockdev_t*)pico_object_ref(&dev->obj);
}

static inline pico_blockdev_t* pico_blockdev_unref(pico_blockdev_t *dev)
{
    return (pico_blockdev_t*)pico_object_unref(&dev->obj);
}

// Debug
#define BLKDEV_INFO(drv, x...) printf(x)
#define BLKDEV_WARN(drv, x...)
#define BLKDEV_ERROR(drv, x...) printf(x)
#define BLKDEV_DEBUG(drv, x...) printf(x)

#endif
