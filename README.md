# Summary
Radeon Rays is ray intersection acceleration library provided by AMD which makes the most of the hardware and allows for efficient ray queries. Three backends support a range of use cases.

*This fork adds/removes stuff to work with AViS. Use this instead of the official repository if you are using it with AViS.*

## Set up OpenCL
Set environmental variable.  GPU_MAX_ALLOC_PERCENT = 100. This is necessary to allocate a large buffers.

## Build                                                                                       

### Windows

Do this instead of the official guide

`cmake -G "Visual Studio 15 2017 Win64"`

`cmake --build . --config <Release or Debug>`

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
