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


### Selecting the demo: UIO or IIO

Two mutually exclusive demo software sets are available as external Buildroot
packages. Enabling `ADCDEMO_UI` hides the IIO packages in the configuration.

| Demo | Package | Data path | Web server |
|------|---------|-----------|------------|
| UIO (default) | `adcdemo-ui` (selects `beaglevfire-adcdemo`) | FPGA -> LSRAM -> UIO/DMA -> mvaring in shared memory | `/ui/adc_server.py` serves the UI and the WebSocket on port 80 (`S50adcdemo_ui`) |
| IIO | `sampler` (selects `sampler-ui`, `sampler-libiio`) | IIO driver `dma-sampler` + libiio | lighttpd serves the UI, `/ui/adc_server.py` the data (`S50adc_server`) |

`sampler_defconfig` selects the UIO demo (`BR2_PACKAGE_ADCDEMO_UI=y`). The UIO
demo leaves the UIO devices bound; the IIO demo (`S99sampler`) unbinds them
and loads the IIO driver.

To switch, run `make menuconfig` in `buildroot/` and, under
*External options*, either

- UIO: enable `adcdemo ui`, or
- IIO: disable `adcdemo ui`, then enable `sampler`.

Alternatively edit `.config` (or `sampler_defconfig` before running
`defconfig`):

```
# UIO
BR2_PACKAGE_ADCDEMO_UI=y
# IIO
# BR2_PACKAGE_ADCDEMO_UI is not set
BR2_PACKAGE_SAMPLER=y
```

Files installed by the previously selected demo stay in `output/target`, so
after switching remove it before building: `rm -rf output/target` and then
run `make` (see below).

Running the UIO demo on the board: open `http://<board-ip>/` in a browser. The
streamer (`/usr/bin/bvf_adc_stream`) is started by the server when sampling is
started from the UI; it can also be run by hand, `bvf_adc_stream -h` lists the
options. The ring buffer is in `/dev/shm/RPI_ADC_BUFF`.


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

Copy the `LinuxProgramming` directory under the `buildroot/output/target`
directory. This requires the image to be rebuilt with `make`.

On the BeagleV-Fire, update the gateware using the provided
`update-gateware.sh` script.

```sh
$ /usr/share/microchip/update-gateware.sh <path-to-LinuxProgramming-directory>
```

The BeagleV-Fire will restart itself after updating.


### Running the UI (host, IIO demo only)

From the project root, run the frontend and server using the `run` script.

```sh
$ ./run frontend
```

```sh
$ sudo ./run server
```
