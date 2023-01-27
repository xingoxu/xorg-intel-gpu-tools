=============
intel_gpu_top
=============

---------------------------------------------
Display a top-like summary of Intel GPU usage
---------------------------------------------
.. include:: defs.rst
:Author: IGT Developers <igt-dev@lists.freedesktop.org>
:Date: 2020-03-18
:Version: |PACKAGE_STRING|
:Copyright: 2009,2011,2012,2016,2018,2019,2020 Intel Corporation
:Manual section: |MANUAL_SECTION|
:Manual group: |MANUAL_GROUP|

SYNOPSIS
========

**intel_gpu_top** [*OPTIONS*]

DESCRIPTION
===========

**intel_gpu_top** is a tool to display usage information on Intel GPU's.

The tool gathers data using perf performance counters (PMU) exposed by i915 and other platform drivers like RAPL (power) and Uncore IMC (memory bandwidth).

OPTIONS
=======

-h
    Show help text.

-J
    Output JSON formatted data.

-l
    List plain text data.

-o <file path | ->
    Output to the specified file instead of standard output.
    '-' can also be specified to explicitly select standard output.

-s <ms>
    Refresh period in milliseconds.
-L
    List available GPUs on the platform.
-d
    Select a specific GPU using supported filter.

RUNTIME CONTROL
===============

Supported keys:

    'q'    Exit from the tool.
    'h'    Show interactive help.
    '1'    Toggle between aggregated engine class and physical engine mode.
    'n'    Toggle display of numeric client busyness overlay.
    's'    Toggle between sort modes (runtime, total runtime, pid, client id).
    'i'    Toggle display of clients which used no GPU time.
    'H'    Toggle between per PID aggregation and individual clients.

DEVICE SELECTION
================

User can select specific GPU for performance monitoring on platform where multiple GPUs are available.
A GPU can be selected by sysfs path, drm node or using various PCI sub filters.

Filter types: ::

    ---
    filter   syntax
    ---
    sys      sys:/sys/devices/pci0000:00/0000:00:02.0
             find device by its sysfs path

    drm      drm:/dev/dri/* path
             find drm device by /dev/dri/* node

    pci      pci:[vendor=%04x/name][,device=%04x][,card=%d]
             vendor is hex number or vendor name

JSON OUTPUT
===========

To parse the JSON as output by the tool the consumer should wrap its entirety into square brackets ([ ]). This will make each sample point a JSON array element and will avoid "Multiple root elements" JSON validation error.

LIMITATIONS
===========

* Not all metrics are supported on all platforms. Where a metric is unsupported it's value will be replaced by a dashed line.

* Non-root access to perf counters is controlled by the *perf_event_paranoid* sysctl.

REPORTING BUGS
==============

Report bugs on fd.o GitLab: https://gitlab.freedesktop.org/drm/igt-gpu-tools/-/issues
