## Custom Buildroot system for BeagleV-Fire


### Obtaining the repository

Clone the repository from GitHub, including the submodules.

```sh
$ git clone -b buildroot https://github.com/RohmSemiconductor/linux-poc.git --recurse-submodules
$ cd linux-poc
```


### Configuring Buildroot

Use the included `sampler_defconfig` defconfig file to configure Buildroot for
BeagleV-Fire boards.

```sh
$ cd buildroot
$ make BR2_EXTERNAL=../package/* BR2_DEFCONFIG=../sampler_defconfig defconfig
```


### Building the image

To build the image, including the tooling, kernel and other packages, run `make`.

```sh
$ make
```

If the build fails because of any missing dependencies, install them and
run `make` again; it will continue from where it left off.


### Installing the image

To prepare the BeagleV-Fire for receiving the image via USB, stop the boot
process by pressing the user button on the board. A tool like `minicom` can be
used to visualise the process.

From the host machine, write the image data with `dd`. Make sure the image
and device paths are the correct ones!

```sh
$ sudo dd if=output/images/sdcard.img of=/dev/sda
```

When the writing has finished, reboot the BeagleV-Fire board.


### SSH'ing to the board

Connect to the board using `ssh`. The root password is simply `root`.

```sh
$ ssh root@192.168.255.27
```


### Updating the gateware

On the BeagleV-Fire, update the gateware using the provided
`update-gateware.sh` script. The `LinuxProgramming` directory
contains the gateware, and can be found in the root directory.

```sh
$ /usr/share/microchip/update-gateware.sh /LinuxProgramming
```

The BeagleV-Fire will restart itself after updating.
