# Client of Application Framework Binder

This project provides simple client of the Application Framework Binder
of micro-service architecture.

This project is available at <https://github.com/redpesk-core/afb-client>

## License and copying

This software if available in dual licensing. See file LICENSE.txt for detail.

## Building

### Requirements

It requires the following libraries:

* libafb (<https://github.com/redpesk-core/afb-libafb>)
* json-c
* systemd >= 222

and the following tools:

* gcc;
* pkg-config;
* cmake >= 3.0

### Simple compilation

The following commands will install the binder in your subdirectory
**$HOME/local** (instead of **/usr/local** the default when
*CMAKE_INSTALL_PREFIX* isn't set).

```sh
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=$HOME/local ..
make install
```

or also:

```sh
CMAKE_INSTALL_PREFIX=$HOME/local ./mkbuild.sh install
```
