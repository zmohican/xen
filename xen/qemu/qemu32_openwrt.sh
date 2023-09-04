# qemu-system-arm -machine virt,virtualization=on,gic_version=2 -cpu cortex-a7 -smp 4 -nographic -m 1024 \

qemu-system-arm -machine orangepi-pc -nographic \
	-device loader,file=/home/zhoux/git/linux/.arm32/arch/arm/boot/zImage,addr=0x50000000 \
	-device loader,file=/home/zhoux/Downloads/openwrt-22.03.5-armvirt-32-rootfs.cpio,addr=0x52000000 \
	-device loader,file=qemu/domU1.dtb,addr=0x53000000 \
	-sd qemu/openwrt-22.03.5-sunxi-cortexa7-xunlong_orangepi-pc-ext4-sdcard.img \
	-kernel .arm32/xen  

#	-device loader,file=/home/zhoux/Downloads/openwrt-22.03.5-armvirt-32-zImage,addr=0x50000000 \
#	-netdev user,id=mynet0 -device virtio-net-device,netdev=mynet0,bus=virtio-mmio-bus.0 

#	-device loader,file=/home/zhoux/nvmeq/Image,addr=0x50000000 \
