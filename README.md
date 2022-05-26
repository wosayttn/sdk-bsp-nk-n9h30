# rt-studio
This is repository of sdk-bsp-nk-n9h30.

# Patch
You need modify rt_mtd_nand_device structure as below for passing the source building using this v1.0.1.

```bash
struct rt_mtd_nand_device
{
    struct rt_device parent;

    rt_uint16_t page_size;          /* The Page size in the flash */
    rt_uint16_t oob_size;           /* Out of bank size */
    rt_uint16_t oob_free;           /* the free area in oob that flash driver not use */
    rt_uint16_t plane_num;          /* the number of plane in the NAND Flash */

    rt_uint32_t pages_per_block;    /* The number of page a block */
    rt_uint16_t block_total;

    /* Only be touched by driver */
    rt_uint32_t block_start;        /* The start of available block*/
    rt_uint32_t block_end;          /* The end of available block */

    /* operations interface */
    const struct rt_mtd_nand_driver_ops *ops;

    void* priv;
};
typedef struct rt_mtd_nand_device* rt_mtd_nand_t;
```