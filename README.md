# raspberrypi-pulse-reader
RaspberryPi kernel driver for reading PPM/PWM duty and cycle. Say if you want to build a car with pi and don't want an additional Arduino board for reading motor speed feedback and RC PPM, you can used this driver.

This module is tested with Raspberry Pi 2 Model B and 4.19.66 kernel. But it should work with other versions.

## How to build
- Download [kernel](https://github.com/raspberrypi/linux) and [toolchain](https://github.com/raspberrypi/tools). Make sure the same kernel version is downloaded(check the VERSION in root make file of kernel source and compare with "uname -r" output of your pi). 
- Follow the instruction to build the kernel.
- Clone the source code of this repo and run below command
```
make KERNEL=<kernel folder> ARCH=arm CROSS_COMPILE=<toolchain foler>/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin/arm-linux-gnueabihf-
```

## Usage
- Copy pulse_reader.ko to pi and run followings. Change the mojor device number if 240 is occupied.(change it in source code as well) 
```
sudo insmod pulse_reader.ko
sudo mknod /dev/pulse_reader c 240 0
```
- The pulse_reader_test.cpp is a simple app for demostrating how to access to the driver. Use below commands to compile it and copy to pi to run.
```
export PATH=$PATH:<toolchain foler>/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin/
arm-linux-gnueabihf-gcc -o pulse_reader_test pulse_reader_test.cpp
```
- Use ADD_IO ioctrl command to insert I/O for monitoring. A pin map for Pi 2 model B could be found [here](https://docs.microsoft.com/en-us/windows/iot-core/media/pinmappingsrpi/rp2_pinout.png). **Causion! Pi 2 model B pins are not 5 volt tolerant. Don't connect 5V signal to the GPIOs**
- User GET_IO_STAT command to get the I/O measurements:
	- duty: positive pulse width in micro-seconds
    - cycle: the cycle time in micro-seconds
- There's a median filter implemented on pulse width. Change filter_win_size to adjust the window size when send command ADD_IO.