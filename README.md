# AWS Neuron Kernel Mode Driver
# Overview

This driver implements a lightweight IOCTL based interface to Neuron devices ([Inferentia](https://aws.amazon.com/machine-learning/inferentia/)) managed by the [Neuron SDK](https://github.com/aws/aws-neuron-sdk).
The interface is used by ML applications and frameworks, such as PyTorch and TensorFlow.

Neuron Device DMA engines are controlled by the driver.
The driver provides functions for copying data between an application running on the host and a Neuron Device.
The driver also provides functions for an application to create rings of DMA descriptors and attach the rings to the DMA engines.
These rings are used to feed instructions and data to Neuron-managed devices during the execution of ML graphs.

Neuron Device Notification engines are controlled by the driver.
The notifications can be generated when a Neuron-managed device executes certain instructions.
The notifications are used to signal task completions to an application and to aid in profiling and debugging.
The driver provides mmap interface to an application to map a buffer in memory used as a notification area.
An application polls the notifications to detect an inference completion.
An application can read the notifications for debugging and profiling.

To improve task execution latency an application can use DMA to send data to a Neuron Device or to receive data from it directly, i.e. the data does not need to pass through the kernel.
The driver provides an interface to allocate coherent memory on behalf of an application.
The memory allocated by the driver can be then used by the application for transferring the data in and out of a Neuron Device.

Multiple Neuron Devices can be connected together via Neuron Pipe technology.
These pipe links enable applications to transfer data from one Neuron Device to another without host CPU intervention.
The driver provides an interface for discovering the neighboring Neuron Devices.

Neuron Devices implement a communication channel (FWIO) that allows the driver and an application to send certain commands to the device.  The communication channel consists of a request and a response queue and a doorbell register.  The communication protocol is implemented by the driver and is used by both the driver and applications.  The driver places a request on the request queue and rings the doorbell.  The driver then polls the response queue.  The polling stops when the Neuron Device places a response on the response queue.  The communication channel is used for infrequent operations that are not latency sensitive such as:

1. Reading Neuron Device registers - the driver avoid direct PCIe reads, instead, the driver issues a read command via FWIO.
2. Writing runtime statistics to a Neuron Device.

# Supported PCI Vendor ID/Device IDs

1d0f:7064  Neuron Inferentia Device
1d0f:7065
1d0f:7066
1d0f:7067

# Directory Structure

* neuron_module.[ch] - Module entry point.
* neuron_pci.[ch] - PCIe BAR setup and Device initialization.
* neuron_ring.[ch] - DMA engine and queue management.
* neuron_dma.[ch] - Provides APIs to copy data from/to device memory.
* neuron_mempool.[ch] - Provides API to allocate host and device memory.
* neuron_cdev.c - char device interface.
* fw_io.[ch] - Communication channel
* udma/* - DMA engines and queues HAL
* v1/address_map.h - Neuron Device address space
* v1/putils.h - Notification HAL
* v1/tdma.h - Additional DMA HAL functionality

# Compiling and Installing

The easiest way to compile and install is using dkms.

### DKMS

```
`cd neuron_driver_directory
dkms add .dkms build
dkms install`
```

### Manual

**Prerequisites - Ubuntu**

```
sudo apt update
sudo apt install build-essential linux-source
```

**Prerequisites - AL2**

```
sudo yum update
sudo yum install "@Development tools" kernel-devel kernel-headers
```

**Compilation**
Run `make` to create neuron.ko in the same folder
**Load**
Use insmod to load the module.

```
`insmod neuron.ko`
```

For automatic driver start upon the OS boot, add neuron to startup modules.

```
`echo "neuron" | tee -a /etc/modules-load.d/neuron.conf
cp neuron.ko /**lib**/**modules**/$(**uname** -**r**)/`
```

To enable applications to access neuron devices without needing root privileges create udev rules.

```
`echo 'KERNEL=="neuron*", MODE="0666"' > /lib/udev/rules.d/neuron-udev.rules`
```


