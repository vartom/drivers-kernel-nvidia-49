ifdef CONFIG_ARCH_TEGRA_19x_SOC

ccflags-y += -Werror

ccflags-y += -I$(srctree)/drivers/iommu
obj-$(CONFIG_ARM_SMMU) += arm-smmu-t19x.o
obj-$(CONFIG_ARM_SMMU) += arm-smmu-regs.o
obj-y += dma-override.o

endif
