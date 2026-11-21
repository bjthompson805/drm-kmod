/* MMIO regmap subset (apple-dart's PMGR syscon access): appledrm_misc.c. */
#ifndef _APPLEDRM_LINUX_REGMAP_H_
#define _APPLEDRM_LINUX_REGMAP_H_
#include <linux/types.h>
#include <linux/err.h>
struct device;
struct regmap;
struct regmap_config {
	const char	*name;
	int		reg_bits;
	int		reg_stride;
	int		val_bits;
	unsigned int	max_register;
	bool		fast_io;
};
struct regmap *regmap_init_mmio(struct device *dev, void __iomem *regs, const struct regmap_config *cfg);
#define devm_regmap_init_mmio(dev, regs, cfg) regmap_init_mmio(dev, regs, cfg)
void regmap_exit(struct regmap *map);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask, unsigned int val);
#endif
