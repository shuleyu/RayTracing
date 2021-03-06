# Download and Install
```
$ git clone --recursive https://github.com/shuleyu/seismic-raytracing.git
```

If `--recursive` is not added, the dependencies will not be downloaded. In such case, do:

```
$ cd ./seismic-raytracing
$ git submodule update --init --recursive
```


# Parameter file (INFILE)
The WORKDIR parameter specify the output folder of this program. The program will create this folder if it doesn't exist.

```
$ vim ./INFILE
```

# Task file (LIST.sh)
Specify which task(s) to execute. Use `#` to comment out unwanted tasks.

```
$ vim ./LIST.sh
```

For now, there's two tasks `a01` (calculation) and `b01` (plotting using GMT4).

# Execution file (Run.sh)
When INFILE and LIST.sh are properly set, run this script:

```
$ bash ./Run.sh
```

This scipt will do several things:

1. create folder WORKDIR
2. create subfolders within WORKDIR:
   - `bin` complied binary files
   - `INPUT` history INFILE(s)
   - `LIST` history LIST.sh(s)
   - `PLOTS` generated figures
   - a bunch of temporary files `tmpfile_XXXX` (they get automatically deleted when the job is finished)
3. try compling the code (if complie failed, program will exit directly. Error message will be printed to the screen)
4. read parameters from INFILE
5. execute tasks specified in LIST.sh

# Example

With a proper WORKDIR, the program will calculate the following example case.

If gmt4 is installed, it will also produce the following figure:

![alt text](https://github.com/shuleyu/raytracing/blob/master/SRC/example1.png)

This is an ScS reflection on an ellipse-shaped low velocity structure on the core mantle boundary.

  - Ray is coming from left hand side.

  - Green lines for polarity change.

  - Line width are comparable to displacement amplitude.

Another example showing multiple ScS reflections on similar structure (varying takeoff angle each time):

![alt text](https://github.com/shuleyu/raytracing/blob/master/SRC/example2.png)

