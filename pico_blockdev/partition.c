#include "pico/blockdev.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct msdos_partition {
    uint8_t boot_ind;
    uint8_t head;
    uint8_t sector;
    uint8_t cyl;
    uint8_t sys_ind;
    uint8_t end_head;
    uint8_t end_sector;
    uint8_t end_cyl;
    uint8_t start_sect[4];
    uint8_t nr_sects[4];
} __attribute__((packed));


typedef struct pico_blockdev_part__
{
    struct pico_blockdev__ dev;
    uint32_t start_sector;
    uint32_t num_sectors;
} pico_blockdev_part_t;


static int pico_blockdev_part_read_sector(pico_blockdev_t *dev, unsigned char* data, uint32_t start_sector, unsigned count);
static int pico_blockdev_part_write_sector(pico_blockdev_t *dev, const unsigned char* data, uint32_t start_sector, unsigned count);
static int pico_blockdev_part_ioctl(pico_blockdev_t *dev, unsigned char cmd, void* data);
static void pico_blockdev_part_destroy(pico_blockdev_t *dev);

static const pico_blockdev_ops_t part_ops =
{
    .read_sector = pico_blockdev_part_read_sector,
    .write_sector = pico_blockdev_part_write_sector,
    .ioctl = pico_blockdev_part_ioctl,
    .destroy = pico_blockdev_part_destroy
};

static void pico_blockdev_part_destroy(pico_blockdev_t *dev)
{
    if (dev->parent)
        pico_blockdev_unref(dev->parent);
    free(dev);
}

static int pico_blockdev_part_read_sector(pico_blockdev_t *dev, unsigned char* data, uint32_t start_sector, unsigned count)
{
    pico_blockdev_part_t *d = (pico_blockdev_part_t*)dev;
    if (d->dev.parent->ops->read_sector)
        return d->dev.parent->ops->read_sector(d->dev.parent, data, start_sector + d->start_sector, count);
    return -ENOSYS;
}

static int pico_blockdev_part_write_sector(pico_blockdev_t *dev, const unsigned char* data, uint32_t start_sector, unsigned count)
{
    pico_blockdev_part_t *d = (pico_blockdev_part_t*)dev;
    if (d->dev.parent->ops->write_sector)
        return d->dev.parent->ops->write_sector(d->dev.parent, data, start_sector + d->start_sector, count);
    return -ENOSYS;
}

static int pico_blockdev_part_ioctl(pico_blockdev_t *dev, unsigned char cmd, void* data)
{
    pico_blockdev_part_t *d = (pico_blockdev_part_t*)dev;
    int r = -1;

    switch (cmd)
    {
    case PICO_IOCTL_BLKGETSIZE:
        *(uint32_t*)data = d->num_sectors;
        r = 0;
        break;
    default:
        if (d->dev.parent->ops->ioctl)
            r = d->dev.parent->ops->ioctl(d->dev.parent, cmd, data);
        break;
    }
    return r;
}

static uint32_t pico_blockdev_extractle32(const uint8_t *src)
{
    uint32_t v = src[0];
    v |= ((uint32_t)src[1])<<8;
    v |= ((uint32_t)src[2])<<16;
    v |= ((uint32_t)src[3])<<24;
    return v;
}

static void pico_blockdev_check_msdos_partition(pico_blockdev_t *dev, uint8_t *source, int index)
{
    struct msdos_partition *p = (struct msdos_partition*)(&source[ index * sizeof(struct msdos_partition) ] );

    if (p->sys_ind != 0x0)
    {
        uint32_t start = pico_blockdev_extractle32(p->start_sect);
        uint32_t size = pico_blockdev_extractle32(p->nr_sects);
        // Allocate new blockdev
        pico_blockdev_part_t *newdev = malloc(sizeof(pico_blockdev_part_t));
        pico_blockdev_init(&newdev->dev, &part_ops);
        newdev->num_sectors = size;
        newdev->start_sector = start;

        int r =pico_blockdev_add_child(dev, &newdev->dev);
        if (r==0)
        {

            BLKDEV_INFO(dev, "New partition found start %d sectors=%d\n", start, size);

            pico_blockdev_register((pico_blockdev_t*)newdev);
        } else
        {
            BLKDEV_ERROR(dev, "Cannot add partition, err %d %s\n", r, strerror(-r));
        }
    }
}

void pico_blockdev_scan_partitions(pico_blockdev_t *dev)
{
    uint8_t sect[512];
    int r = pico_blockdev_read_sector(dev, sect, 0, 1);
    if (r==1) {
        if (sect[510]==0x55 && sect[511]==0xAA)
        {
            BLKDEV_DEBUG(dev, "Found MSDOS partition table, scanning partitions\n");

            for (int i=0; i<4; i++) {
                pico_blockdev_check_msdos_partition(dev, &sect[0x1be], i);
            }
        }
    } else {
        BLKDEV_ERROR(dev, "Cannot read first sector to read partition"
                     "table, error %d", r);
    }
}
