#include "pico/blockdev.h"
#include <stdlib.h>
#include <sys/errno.h>

extern void pico_blockdev_scan_partitions(pico_blockdev_t *dev);

static void pico_blockdev_destroy_object(pico_object_t *obj)
{
    pico_blockdev_t *dev = (pico_blockdev_t*)obj;
    // Called after unref.
    if (dev && dev->ops && dev->ops->destroy) {
        dev->ops->destroy(dev);
    }
}

int pico_blockdev_read_sector(pico_blockdev_t *dev, unsigned char* data, uint32_t start_sector, unsigned count)
{
    if (dev->ops->read_sector) {
        return (*dev->ops->read_sector)(dev, data, start_sector, count);
    } else {
        return -ENOSYS;
    }
}

int pico_blockdev_write_sector(pico_blockdev_t *dev, const unsigned char* data, uint32_t start_sector, unsigned count)
{
    if (dev->ops->write_sector) {
        return (*dev->ops->write_sector)(dev, data, start_sector, count);
    } else {
        return -ENOSYS;
    }
}

int pico_blockdev_ioctl(pico_blockdev_t *dev, unsigned char cmd, void* data)
{
    if (dev->ops->ioctl) {
        return (*dev->ops->ioctl)(dev, cmd, data);
    } else {
        return -ENOSYS;
    }
}

int pico_blockdev_init(pico_blockdev_t *dev, const pico_blockdev_ops_t *ops)
{
    pico_object_init(&dev->obj, &pico_blockdev_destroy_object);
    dev->ops = ops;
    dev->children = NULL;
    dev->parent = NULL;
    return 0;
}

bool pico_blockdev_has_children(pico_blockdev_t *dev)
{
    return dev->children != NULL;
}

void __attribute__((weak)) pico_blockdev_register_event(pico_blockdev_t *dev)
{
}
void __attribute__((weak)) pico_blockdev_unregister_event(pico_blockdev_t *dev)
{
}

int pico_blockdev_register(pico_blockdev_t *dev)
{
    if (!dev->parent)
    {
        pico_blockdev_scan_partitions(dev);
    }
    pico_blockdev_register_event(dev);
    pico_blockdev_unref(dev);

    return 0;
}

void pico_blockdev_unregister(pico_blockdev_t *dev)
{
    while (dev && dev->children)
    {
        struct pico_blockdev_link_entry *link = dev->children;
        pico_blockdev_unregister(link->dev);
        dev = pico_blockdev_unref(dev);
        if (dev)
        {
            dev->children = link->next;
        }
    }
    pico_blockdev_unregister_event(dev);
}

int pico_blockdev_add_child(pico_blockdev_t *dev, pico_blockdev_t *child)
{
    struct pico_blockdev_link_entry *link;

    if (child->parent!=NULL) {
        return -EALREADY;
    }

    link = malloc(sizeof(struct pico_blockdev_link_entry));

    if (NULL==link)
        return -ENOMEM;

    pico_object_lock(&dev->obj);

    link->dev = child;
    link->next = dev->children;
    dev->children = link;
    child->parent = dev;

    // Reference
    pico_object_ref(&child->obj);
    pico_object_ref_nolock(&dev->obj);

    pico_object_unlock(&dev->obj);

    return 0;
}

