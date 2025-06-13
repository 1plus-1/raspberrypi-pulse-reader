# raspberrypi-pulse-reader
RaspberryPi kernel driver for reading PPM/PWM duty and cycle. Say if you want to build a car with pi and don't want an additional Arduino board for reading motor speed feedback and RC PPM, you can used this driver.

This module has been tested with Raspberry Pi 2 Model B and Raspberry Pi 4 Model B with Raspberry Pi OS + kernel 6.6.51.

## How to build
### Compile on Pi
- Clone the source code to pi and run following commands.
	```
	sudo apt update && sudo apt install build-essential raspberrypi-kernel-headers -y
	cd raspberrypi-pulse-reader/pulse_reader_module
	make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
	```
### Cross Compiling
- Download [kernel](https://github.com/raspberrypi/linux). Make sure the same kernel version is downloaded(check the VERSION in root make file of kernel source and compare with "uname -r" output of your pi). 
- Follow the [instruction](https://www.raspberrypi.org/documentation/linux/kernel/building.md) to build the kernel.
- Clone the source code of this repo and run below command to build the .ko file
	```
	make KERNEL=<kernel folder> ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
	```
## Usage
- Copy pulse_reader.ko to pi and run insmod:
	```
	sudo insmod pulse_reader.ko
	```
- Or load module at system startup:
	```
	sudo cp pulse_reader.ko /lib/modules/$(uname -r)/
	sudo echo pulse_reader > /etc/modules-load.d/pulse_reader.conf
	sudo depmod -a
	sudo reboot
	```
- Change the mojor device number in module.c if 240 is occupied.
- The pulse_reader_test.cpp is a simple app for demostrating how to access to the driver. Use below commands to compile it and copy to pi to run.
	```
	arm-linux-gnueabihf-gcc -o pulse_reader_test pulse_reader_test.cpp
	```
- Use ADD_IO ioctrl command to insert I/O for monitoring. A pin map for Pi 2 model B could be found [here](https://docs.microsoft.com/en-us/windows/iot-core/media/pinmappingsrpi/rp2_pinout.png). **Causion! Pi 2 model B pins are not 5 volt tolerant. Don't connect 5V signal to the GPIOs**
- User GET_IO_STAT command to get the I/O measurements:
	- duty: positive pulse width in micro-seconds
    - cycle: the cycle time in micro-seconds
- There's a median filter implemented on pulse width. Change filter_win_size to adjust the window size when send command ADD_IO.
