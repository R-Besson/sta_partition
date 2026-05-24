YOSYS PLUGIN: STA_PARTITION
---------------------------

GOAL:
----
The goal of this project and this repo is to provide a way to call OpenSTA on a big netlist and reduce drastically 
the overall runtime to get the timing report.
To do so we plan to partition the netlist and call OpenSTA on each partition, get all the timing reports associated 
to each partition and merge them all to get a single timing report. This timing report should be "close" to the one
we would have get if we would have called Open Sta on the original netlist.

Of course the way the partitioning will be performed is very important because we want OpnSta to still catch all 
the timing paths, especially the critical ones.
Therefore this means we need to partitioning in a very specific way.


REQUIREMENTS:
------------
To achieve this project we need to use and install several tools :

    1/ Yosys : this is the synthesis tool and this one will be necessary to represent internally 
    in memory the netlist data-structure. This data-structure is called RTLIL. We also use its 
    "verilog" parser to read the verilog netlist and  build the internal netlist in memory.
       --> https://github.com/YosysHQ/yosys

    2/ Open Sta: Open STA is the "Static Timing Analysis" tool used to get report timings on the 
    netlist.
       --> https://github.com/The-OpenROAD-Project/OpenSTA

    3/ Slang : eventually the "slang" open source plugin providing a robust system verilog parser 
    and elaborator.
       --> https://github.com/MikePopoloski/slang
 

BUILDING THE PLUGIN:
--------------------
In order to build the "sta_partition" plugin, the best way is to build it related to the Yosys 
tree that you previously cloned, so that it will be placed in the right plugin directory of 
the Yosys tree.

We use "cmake" for that so you need to call the following commands :

cmake -S . -B build -D YOSYS_TREE=<path_where_yosys_root_dir_is_located>; 
cmake --build build; cmake --install build"

-----------

