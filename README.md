# Summary
Radeon Rays is ray intersection acceleration library provided by AMD which makes the most of the hardware and allows for efficient ray queries. Three backends support a range of use cases.

## Set up OpenCL
Set environmental variable.  GPU_MAX_ALLOC_PERCENT = 100. This is necessary to allocate a large buffers.

## Build                                                                                       

### Windows

`cmake -G "Visual Studio 14 2015 Win64"`

### OSX

`brew install homebrew/science/openimageio`

`mkdir build`

`cd build`

`cmake -DCMAKE_BUILD_TYPE=<Release or Debug> ..` 

`make`

### Linux

`sudo apt-get install libopenimageio-dev libglew-dev freeglut3-dev`

`mkdir build`

`cd build`

`cmake -DCMAKE_BUILD_TYPE=<Release or Debug> ..`

`make`

### Options
Available premake options:
- `RR_USE_EMBREE` will enable the embree backend. Embree device will be the last one in IntersectionApi device list.
 example of usage : 
 `cmake -DCMAKE_BUILD_TYPE=<Release ro Debug> -DRR_USE_EMBREE=ON ..`

- `RR_USE_OPENCL` will enable the OpenCL backend. If no other option is provided, this is the default

- `RR_SHARED_CALC` will build Calc (Compute Abstraction Layer) as a shared object. This means RadeonRays library does not directly depend on OpenCL and can be used on the systems where OpenCL is not available (with Embree backend). 

- `RR_EMBED_KERNELS` will include CL code as header instead of text file.

## Run unit tests
They need to be run from the <Radeon Rays_SDK path>/UnitTest path.
CMake should be runned with the `RR_SAFE_MATH` option.
