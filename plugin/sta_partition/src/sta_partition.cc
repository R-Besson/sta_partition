// Yosys plugin STA_PARTITION
//

#include "kernel/celltypes.h"
#include "kernel/ff.h"
#include "kernel/ffinit.h"
#include "kernel/log.h"
#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "kernel/gzip.h"
#include "kernel/sigtools.h"
#include "passes/techmap/libparse.h"
#include <csignal>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdexcept>
#include <deque>
#include <thread>
#include <mutex>

#define NB_PARTITIONS_DEFAULT 16

#define INPUTS(x) get<0>(x)
#define CELL(x)   get<1>(x)

namespace fs = std::filesystem;

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Data structure to quickly manipulate netlist cells and nets
//
// See explanation of type "Sinks" with a schematic example below.
//
// The "std::tuple<Cell*, IdString, int>" is a "sink" which represents :
//
//   Cell/PortName/Bus_index
//
//   if portName is a single bit signal.
//
typedef dict<RTLIL::SigSpec, vector<std::tuple<Cell*, IdString, int>>*> Sinks;

/*
    // Based on the schematic below, "Sinks" will store for each
    // "net" of type RTLIL::SigSpec something which looks like that :
    //
    //       <net1, <{CO,"A",0}>>
    //       <net2, <{CO,"B",0}>>
    //       <net3, <{C1,"B",0}, {C2,"A",0}, {C3,"A",0}>>
    //
      ┌────────────┐
      │ Cell D1    │[Y]──────┐
      └────────────┘         │net1
                             │                                 ┌─────────┐
                             │     ┌──────────┐             [A]│         │
                             └─►[A]│          │     net3       │ Cell C1 │
      ┌────────────┐               │ Cell C0  │►[Y]─────┬──►[B]│         │
      │ Cell D2    │[Y]────────►[B]│          │         │      └─────────┘
      └────────────┘     net2      └──────────┘         │      ┌─────────┐
                                                        ├──►[A]│         │
                                                        │      │ Cell C2 │
                                                        │   [B]│         │
                                                        │      └─────────┘
                                                        │      ┌─────────┐
                                                        └──►[A]│         │
                                                               │         │
                                                            [B]│ Cell C3 │
                                                               │         │
                                                            [C]│         │
                                                               └─────────┘

*/


// -------------------------------
// cleanup_all
// -------------------------------
//
void cleanup_all()
{
}

// ---------------------------------------------------------------------------
// handle_sigint
// ---------------------------------------------------------------------------
void handle_sigint(int)
{
   fprintf(stderr, "\n Synthesis interrupted with <ctrl-C> !\n");
   cleanup_all();
   std::_Exit(0);
}

// ---------------------------------------------------------------------------
// handle_sigsegv
// ---------------------------------------------------------------------------
void handle_sigsegv(int s)
{
   fprintf(stderr, "\n");
   fprintf(stderr, "ERROR: Abort due to internal error in Synthesis. Please contact ZeroAsic support.\n");
   cleanup_all();
   std::_Exit(0);
}


// ------------------------------------------------------------
// StaPartition
// ------------------------------------------------------------
struct StaPartition : public ScriptPass {
 
  int nb_partitions = NB_PARTITIONS_DEFAULT; 

  string project = "";

  string top_module_name = "";

  vector<string> verilog_files = {};
  string vlg_name;

  vector<string> liberty_files = {};
  string lib_name;

  vector<string> abc_liberty_files = {};
  string abc_lib_name;

  vector<string> map_libs = {};
  string maplib_name;

  vector<string> sdc_files = {};
  string sdc_name;

  vector<string> libertys = {};
  string liberty_name;

  vector<string> dont_use = {};
  string dont_use_name;

  vector<string> abc_dont_use = {};
  string abc_dont_use_name;

  string dont_use_file_name = "";
  vector<string> dont_use_files = {};

  string liberty_leakage_power_unit = "";
  string leakage_power_unit = "";

  bool slang; 

  int show_liberty_comb_cells;

  // Liberty file related
  //
  struct liberty_cell_info {
        double area;
        float cell_leak_power;
        bool is_sequential;
        vector<double> single_parameter_area;
        vector<vector<double>> double_parameter_area;
        vector<string> parameter_names;
  };

  pool<IdString> g_liberty_buffers;

  pool<IdString> g_liberty_registers;
  dict<string, string> g_liberty_registers_clk_pin_name;

  pool<IdString> g_liberty_combinationals;

  // cell name -> cell info
  //
  dict<IdString, liberty_cell_info> g_liberty_cells_info;

  // cell Boolean function -> vector of cells matching this Boolean function
  //
  // the vector of cells is ordered from min output capacitance to max output capacitance.
  //
  dict<string, vector< pair<string, float> >> g_func_cell_families;
  
  // Cell name to func string
  //
  dict<string, string> g_cell_func;

  string liberty_time_unit = "";
  bool liberty_time_unit_different_units = false;
  string time_unit = "";
  int time_unit_coef = 1;
  int freq_unit_coef = 1; // for 1 ps

  // --------------------------------------------------------------------------------
  //
  StaPartition() : ScriptPass("sta_partition", "partition design, run Open Sta on each of them, and return merged timing reports") {}

  void help() override {
     log("\n");
     log("    sta_partition\n");
     log("\n");
  }

  // ----------------------------
  // remove_if_exists
  // ----------------------------
  //
  void remove_if_exists(const fs::path& dir)
  {
     try
     {
        if (!fs::exists(dir)) {
          return;  // nothing to remove
        }

        fs::remove_all(dir);  // delete directory and all contents
        return;
     }

     catch (const fs::filesystem_error& e)
     {
        std::cerr << "Filesystem error 'remove_if_exists': " << e.what() << '\n';
        exit(1);
        return;
     }
  }

  // ------------------------------
  // isFloat
  // ------------------------------
  //
  bool isFloat(const std::string& s) {

     try {
        size_t pos;
        std::stof(s, &pos);
        return pos == s.length();

     } catch (...) {
        return false;
     }
  }

  // ----------------------------
  // clear_flags
  // ----------------------------
  //
  void clear_flags() override {

    nb_partitions = NB_PARTITIONS_DEFAULT; 

    project = "";

    top_module_name = "";

    vlg_name = "";
    verilog_files.clear();

    lib_name = "";
    liberty_files.clear();

    abc_lib_name = "";
    abc_liberty_files.clear();

    maplib_name = "";
    map_libs.clear();

    sdc_name = "";
    sdc_files.clear();

    dont_use_file_name = "";
    dont_use_files.clear();

    libertys.clear();
    liberty_name = "";

    dont_use.clear();
    dont_use_name = "";

    abc_dont_use.clear();
    abc_dont_use_name = "";

    dont_use_file_name = "";
    dont_use_files.clear();

    liberty_leakage_power_unit = "";
    leakage_power_unit = "";

    slang = false;
  }


  // ----------------------------
  // clear_actual_users
  // ----------------------------
  // We need to write a specific clean function because objects in the dictionary have
  // been allocated with "new".
  //
  static void clear_actual_users (Sinks* actual_users)
  {
     for (auto au : *actual_users) {
       delete(au.second); // delete the allocated vector with
                          // "new vector<std::tuple<Cell*, IdString, int>>"
     }

     actual_users->clear();
  }


  // ----------------------------
  // build_design_dictionaries
  // ----------------------------
  //
  static void build_design_dictionaries (Sinks* actual_users, 
                                         dict<IdString, Cell*>* string_cells)
  {
     // Clean up the dictionaries
     //
     clear_actual_users(actual_users);
     string_cells->clear();

     // Visit all the cells input connections and build :
     //
     //    1. the dictionary 'actual_users'
     //    2. the dictionary 'string_cells' (cell->name to Cell*)
     //
     Design* design = yosys_get_design();
     Module* top_module = design->top_module();

     for (auto cell : top_module->cells()) {

        // Build 'string_cells'
	//
        (*string_cells)[cell->name] = cell;

	// Build 'actual_users'
	//
        for (auto &conn : cell->connections()) {

           IdString portName = conn.first;

           if (!cell->input(portName)) {
              continue;
           }

           RTLIL::SigSpec actual = conn.second;

	   for (int i = 0; i < actual.size(); i++) {

               vector<std::tuple<Cell*, IdString, int>>* vec;

               SigSpec bit_sig = actual.extract(i, 1);

               if (actual_users->find(bit_sig) == actual_users->end()) {

                   // Use "new" : make sure to "delete" later to avoid memory
                   // leaks.
                   //
                   vec = new vector<std::tuple<Cell*, IdString, int>>;

                   (*actual_users)[bit_sig] = vec;

               } else {

                  vec = (*actual_users)[bit_sig];
               }

	       // 'vect' of the "bit_sig"
	       //
	       // Store where this 'bit_sig' is used in the 'actual'
	       // e.g, the cell, the portName and the index in the
	       // bus.
	       //
               vec->push_back(std::make_tuple(cell, portName, i));
           }

        } // end for all the connections of the cell

     } // end for all the cells
  }

  // ----------------------------
  // wait_for_sta_to_reach
  // ----------------------------
  // Wait function used by the STA caller to wait for reaching the setp of name
  // "step_file_name".
  //
  // Basically when STA completes a step "STEP" it will create a file of name "STEP".
  // So finding the presence of file "STEP" for the caller, means that STA has
  // completed this stp.
  //
  void wait_for_sta_to_reach(string step_file_name)
  {
     while (1) {

         std::ifstream f(step_file_name.c_str());

         if (f.good()) {
           return;
         }

         sleep(1);
     }
  }

  // -------------------------------
  // run_tail
  // -------------------------------
  // Mimic : system("tail -f /dev/null > sta_in &");
  //
  void run_tail()
  {
     pid_t pid = fork();

     if (pid == 0) {  // child process
        int fd = open("sta_in", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0) _exit(1);

        // redirect stdout to sta_in
        dup2(fd, STDOUT_FILENO);
        close(fd);

        execlp("tail", "tail", "-f", "/dev/null", (char*)nullptr);

        _exit(1); // only if exec fails
     }

     // parent continues immediately (background behavior)
     return ;
  }


  // -------------------------------
  // run_sta
  // -------------------------------
  // Mimic : system("sta < sta_in > sta_out.log &");
  //
  void run_sta()
  {
    pid_t pid = fork();

    if (pid == 0) {  // child process

        // open input file
        int in_fd = open("sta_in", O_RDONLY);
        if (in_fd < 0) _exit(1);

        // open output file
        int out_fd = open("sta_out.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) _exit(1);

        // redirect stdin
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);

        // redirect stdout
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);

        // execute "sta" (searched via PATH)
        char* args[] = {(char*)"sta", nullptr};
        execvp("sta", args);

        _exit(1); // only reached if exec fails
    }

    // parent does NOT wait -> background behavior
    return;
  }


  // ----------------------------------
  // set_liberty_time_unit
  // ----------------------------------
  //
  void set_liberty_time_unit(string& value, string liberty_file)
  {
    if (liberty_time_unit == "") {
      liberty_time_unit = value;
      return;
    }

    if (liberty_time_unit == value) {
      return;
    }

    log_warning("Found different time units : '%s' vs. '%s' (last liberty file '%s')\n",
                 liberty_time_unit, value, liberty_file);

    // remember that we found this "different time units" problem in the .libs
    // so that we will report it at the end because it may screw up may things
    // (ex: SDC is assuming a different time unit as the default one we will
    // pick up here).
    //
    liberty_time_unit_different_units = true;

    log_warning("Keep unit '%s' as reference unit.\n", liberty_time_unit);

#if 0
    // Overide always with smaller time unit (ex: 1ps is smaller than 1ns).
    // if 'value' time unit is smaller than current 'liberty_time_unit' then take
    // it as new 'liberty_time_unit'.
    //
    if (smaller_time_unit_value(liberty_time_unit, value)) {

      log_warning("Found different time units : '%s' vs. '%s' (last liberty file '%s')\n",
                   liberty_time_unit, value, liberty_file);

      liberty_time_unit = value;

      log_warning("Using unit '%s' as new reference unit.\n", liberty_time_unit);
    }
#endif
  }


  // ----------------------------------
  // get_clock_pin_name
  // ----------------------------------
  int get_clock_pin_name(LibertyAst *reg, string& clk_pin)
  {
    clk_pin = "";

    for (auto pin : reg->children) {

       const LibertyAst *dir = pin->find("direction");

       if (!dir || dir->value != "input") {
           continue;
       }

       std::string pin_name = pin->args[0];

       const LibertyAst *clock = pin->find("clock");

       if (clock) {

           std::string value = clock->value;

           if (value == "true") {

              clk_pin = pin_name;
              return 1;
           }
       }
    }

    return 0;
  }

  // ---------------------------------------------------------------------------
  // analyze_liberty_file
  // ---------------------------------------------------------------------------
  // Collect all the BUFFER cells, FLIP-FLOPS cells and Combinational cells 
  // in the liberty file.
  // For the combinational cells, store each same Boolean function cells into a
  // same bucket. These buckets will be sorted later on (when all liberty files 
  // will be processed) from lowest capacitance to highest.
  //
  void analyze_liberty_file(string& liberty_file)
  {
    std::istream *f = uncompressed(liberty_file.c_str());
    yosys_input_files.insert(liberty_file);
    LibertyParser libparser(*f, liberty_file);
    delete f;
    pool<LibertyAst*> liberty_buffers;
    pool<LibertyAst*> liberty_combinationals;

    for (LibertyAst* cell : libparser.ast->children) {

       if (cell->id != "cell" || cell->args.size() != 1) {
          continue;
       }

       bool dontuse = cell->find("dont_use") != nullptr;

       if (dontuse) {

	  string dont_use_lib_cell_name = cell->args[0].c_str();

          dont_use.push_back(dont_use_lib_cell_name);

          abc_dont_use.push_back(dont_use_lib_cell_name);

          continue;
       }

       bool is_flip_flop = cell->find("ff") != nullptr;

       if (is_flip_flop) {

          g_liberty_registers.insert(RTLIL::escape_id(cell->args[0].c_str()));

          string clk_pin_name;

          if (get_clock_pin_name(cell, clk_pin_name)) {
             g_liberty_registers_clk_pin_name[cell->args[0].c_str()] = clk_pin_name;
	  }
	  continue;
       }

       for (auto pin : cell->children)
       {
         const LibertyAst *dir = pin->find("direction");

         if (!dir || dir->value != "output") {
             continue;
	 }

         const LibertyAst *func = pin->find("function");

         if (func) {

             std::string value = func->value;

	     if ((value.size() == 1) && (value != "0") && (value != "1"))  {
               g_liberty_buffers.insert(RTLIL::escape_id(cell->args[0].c_str()));
	       liberty_buffers.insert(cell);
               continue;
	     }

	     if ((value.size() == 3) && (value.substr(0,1) == "(") && (value.substr(2,1) == ")"))  {
               g_liberty_buffers.insert(RTLIL::escape_id(cell->args[0].c_str()));
	       liberty_buffers.insert(cell);
               continue;
	     }

             g_liberty_combinationals.insert(RTLIL::escape_id(cell->args[0].c_str()));
             liberty_combinationals.insert(cell);
         }
       }
    }

    // Place the previous extracted "liberty_combinationals" cells in the right buckets.
    //
    for (auto cell : liberty_combinationals) {

       for (auto pin : cell->children)
       {
         const LibertyAst *dir = pin->find("direction");

         if (!dir || dir->value != "output") {
             continue;
	 }

         const LibertyAst *func = pin->find("function");

         if (func) {

             std::string value = func->value;

	     const LibertyAst *capacitance = pin->find("max_capacitance");

	     string capa = "0.0";

	     if (capacitance) {
                capa = capacitance->value;
	     }

             if (!isFloat(capa)) {
                log_error("A capacitance could not be exracted.\n");
             }
	     float max_cap = stof(capa);

             if (g_func_cell_families.find(value) == g_func_cell_families.end()) {
                g_func_cell_families[value] = std::vector<pair <string,float>>{};
	     }

	     string cell_name = cell->args[0];

	     g_func_cell_families[value].push_back({cell_name, max_cap});

	     g_cell_func[cell_name] = value;
	 }
       }
    }

    // Place the previous extracted "liberty_buffers" cells in the right buckets.
    //
    for (auto cell : liberty_buffers) {

       for (auto pin : cell->children)
       {
         const LibertyAst *dir = pin->find("direction");

         if (!dir || dir->value != "output") {
             continue;
         }

         const LibertyAst *func = pin->find("function");

         if (func) {

             std::string value = func->value;

             const LibertyAst *capacitance = pin->find("max_capacitance");

             string capa = "0.0";

             if (capacitance) {
                capa = capacitance->value;
             }

             if (!isFloat(capa)) {
                log_error("A capacitance could not be exracted.\n");
             }
             float max_cap = stof(capa);

             if (g_func_cell_families.find(value) == g_func_cell_families.end()) {
                g_func_cell_families[value] = std::vector<pair <string,float>>{};
             }

             string cell_name = cell->args[0];

             g_func_cell_families[value].push_back({cell_name, max_cap});

             g_cell_func[cell_name] = value;
         }
       }
    }
  }

  // ----------------------------------
  // set_time_unit_coefs
  // ----------------------------------
  //
  void set_time_unit_coefs()
  {
    if (liberty_time_unit == "") {
      return;
    }

    if (liberty_time_unit == "1ps") {
      time_unit = "ps";
      time_unit_coef = 1;
      freq_unit_coef = 1;
      return;
    }

    if (liberty_time_unit == "1ns") {
      time_unit = "ns";
      time_unit_coef = 1;
      freq_unit_coef = 1000;
      return;
    }

    if (liberty_time_unit == "10ps") {
      time_unit = "ps";
      time_unit_coef = 10;
      freq_unit_coef = 10;
      return;
    }

    if (liberty_time_unit == "100ps") {
      time_unit = "ps";
      time_unit_coef = 100;
      freq_unit_coef = 100;
      return;
    }

    if (liberty_time_unit == "1000ps") {
      time_unit = "ns";
      time_unit_coef = 1;
      freq_unit_coef = 1000;
      return;
    }

    if (liberty_time_unit == "10ns") {
      time_unit = "ns";
      time_unit_coef = 10;
      freq_unit_coef = 10000;
      return;
    }

    if (liberty_time_unit == "100ns") {
      time_unit = "ns";
      time_unit_coef = 100;
      freq_unit_coef = 100000;
      return;
    }

    log_warning("Do not know how to convert liberty file time unit '%s'\n", liberty_time_unit);
  }

  // ----------------------------------
  // set_liberty_leakage_power_unit
  // ----------------------------------
  //
  void set_liberty_leakage_power_unit(string& value, string liberty_file)
  {
    if (liberty_leakage_power_unit == "") {
      liberty_leakage_power_unit = value;
      return;
    }

    if (liberty_leakage_power_unit == value) {
      return;
    }

    log_warning("Found different leakage power units : '%s' vs. '%s' (last liberty file '%s')\n",
                liberty_leakage_power_unit, value, liberty_file);
    log_warning("Keep unit '%s' as reference unit.\n", liberty_leakage_power_unit);
  }

  // ----------------------------------
  // set_leakage_power_unit
  // ----------------------------------
  //
  void set_leakage_power_unit()
  {
    if (liberty_leakage_power_unit == "") {
      return;
    }

    if (liberty_leakage_power_unit == "1pW") {
      leakage_power_unit = "pW";
      return;
    }

    if (liberty_leakage_power_unit == "1uW") {
      leakage_power_unit = "uW";
      return;
    }

    if (liberty_leakage_power_unit == "1mW") {
      leakage_power_unit = "mW";
      return;
    }

    log_warning("Do not know how to convert liberty file leakage_power unit '%s'\n",
                liberty_leakage_power_unit);
  }


  // ----------------------------------
  // create_liberty_cells_info
  // ----------------------------------
  //
  // Code picked up from "passes/cmds/stat.c"
  //
  void create_liberty_cells_info(string liberty_file)
  {
    std::istream *f = uncompressed(liberty_file.c_str());
    yosys_input_files.insert(liberty_file);
    LibertyParser libparser(*f, liberty_file);
    delete f;

    for (auto cell : libparser.ast->children) {

       if (cell->id == "time_unit") {
         std::string value = cell->value;
	 set_liberty_time_unit(value, liberty_file);
         set_time_unit_coefs();
         continue;
       }

       if (cell->id == "leakage_power_unit") {
         std::string value = cell->value;
	 set_liberty_leakage_power_unit(value, liberty_file);
         set_leakage_power_unit();
         continue;
       }

       if (cell->id != "cell" || cell->args.size() != 1) {
          continue;
       }

       const LibertyAst *ar = cell->find("area");

       bool is_flip_flop = cell->find("ff") != nullptr;
       vector<double> single_parameter_area;
       vector<vector<double>> double_parameter_area;
       vector<string> port_names;
       const LibertyAst *sar = cell->find("single_area_parameterised");

       if (sar != nullptr) {

           for (const auto &s : sar->args) {

              if (s.empty()) {
                 //catches trailing commas
                 continue;
              }

              try {
                 double value = std::stod(s);
                 single_parameter_area.push_back(value);
              } 
	      catch (const std::exception &e) {
                 log_error("Failed to parse single parameter area value '%s': %s\n", s, e.what());
              }
           }

           if (single_parameter_area.size() == 0) {

             log_error("single parameter area has size 0: %s\n", sar->args[single_parameter_area.size() - 1]);
             // check if it is a double parameterised area
	   }
       }

       const LibertyAst *dar = cell->find("double_area_parameterised");

       if (dar != nullptr) {

          for (const auto &s : dar->args) {

              vector<string> sub_array;
              std::string::size_type start = 0;
              std::string::size_type end = s.find_first_of(",", start);

              while (end != std::string::npos) {
                 sub_array.push_back(s.substr(start, end - start));
                 start = end + 1;
                 end = s.find_first_of(",", start);
              }

              sub_array.push_back(s.substr(start, end));
              vector<double> cast_sub_array;

              for (const auto &s : sub_array) {

                 double value = 0;

                 if (s.empty()) {
                   //catches trailing commas
                   continue;
                 }
                 try {
                    value = std::stod(s);
                    cast_sub_array.push_back(value);
                 } 
	         catch (const std::exception &e) {
                    log_error("Failed to parse double parameter area value for  '%s': %s\n", 
			        s, e.what());
                 }
              }

              double_parameter_area.push_back(cast_sub_array);

              if (cast_sub_array.size() == 0) {
                log_error("double paramter array has size 0: %s\n", s);
	      }
          }
       }

       const LibertyAst *par = cell->find("port_names");

       if (par != nullptr) {

          for (const auto &s : par->args) {
             port_names.push_back(s);
          }
       }

       const LibertyAst *lp = cell->find("cell_leakage_power");
       float leak_power = -1.0;  // undefined so far
       if (lp != nullptr) { 
          leak_power = atof(lp->value.c_str());
       }

       // If we do not have direct cell_leakage_power then we average it
       // on all the "leakage_power" cases.
       //
       if (leak_power <= 0) {

          int nb_cases = 0;

          for (LibertyAst* child : cell->children) {

             if (child->id != "leakage_power") {
                continue;
             }
             const LibertyAst *leak_ast = child->find("value");

             string leak = leak_ast->value;

	     leak_power += stof(leak);
             nb_cases++;
          }

          if (nb_cases) {
	     leak_power = leak_power / nb_cases;
	  }
       }

       if (cell->id != "cell" || cell->args.size() != 1) {
          continue;
       }

       if (ar != nullptr && !ar->value.empty()) {

          string prefix = cell->args[0].substr(0, 1) == "$" ? "" : "\\";

	  string cell_name = prefix + cell->args[0];

          g_liberty_cells_info[cell_name] = {atof(ar->value.c_str()), 
                                                          leak_power,
                                                          is_flip_flop, 
                                                          single_parameter_area, 
							  double_parameter_area, 
							  port_names};
       }
    }

  }

  // -----------------------------------
  // sort_cells_bucket
  // -----------------------------------
  // Sort cells in the bucket from lowest max cap to highest.
  //
  void sort_cells_bucket(std::vector<std::pair<string, float>>& v)
  {
    const float eps = 1e-6f;

    std::sort(v.begin(), v.end(),
        [eps](const std::pair<std::string, float>& a,
              const std::pair<std::string, float>& b)
        {
            // If floats are "close enough", use string tie-breaker
            if (std::fabs(a.second - b.second) < eps)
                return a.first < b.first;

            return a.second < b.second; // ascending by float
        });
  }

  // ---------------------------------------------------------------------------
  // analyze_all_liberty_files
  // ---------------------------------------------------------------------------
  //
  void analyze_all_liberty_files (vector<string> &libertys)
  {
     log_header(yosys_get_design(), "Extracting liberty files info ...\n");

     // Extract cells info
     //
     for (auto liberty_file : libertys) {

        rewrite_filename(liberty_file);

        analyze_liberty_file(liberty_file);

	create_liberty_cells_info(liberty_file);
     }

     // In each combinational cells bucket (they have the same Boolean function), 
     // sort cells according to their max capacitance from low to high.
     //
     for (auto& cell_bucket : g_func_cell_families) {

         sort_cells_bucket(cell_bucket.second);
     }

#if 0
     // show cells buckets
     //
     if (show_liberty_comb_cells) {

        for (auto cell_bucket : g_func_cell_families) {

            log("Cell bucket function : %s\n", cell_bucket.first);

            for (auto cell_bucket : cell_bucket.second) {
               log("      Cell '%s', Max Cap = %f\n", cell_bucket.first, cell_bucket.second);
            }

        }

        log(" Press ENTER to continue ... \n");
        getchar();
        log(" Resuming ... \n");
     }
#endif

  }

  // ----------------------------
  // execute
  // ----------------------------
  //
  void execute(std::vector<std::string> args, RTLIL::Design *design) override {

     string run_from, run_to;

     log_header(design, "Executing STA PARTITION\n");

     clear_flags();

     size_t argidx;

     for (argidx = 1; argidx < args.size(); argidx++) {


        if (args[argidx] == "-project" && argidx + 1 < args.size()) {
          if (project != "") {
             log_error("Option -project must be set only once.\n");
          }
          project = args[++argidx];
          continue;
        }

        if (args[argidx] == "-top" && argidx + 1 < args.size()) {
          if (top_module_name != "") {
             log_error("Option -top must be set only once.\n");
          }
          top_module_name = args[++argidx];
          continue;
        }

        if (args[argidx] == "-nb_partitions" && argidx + 1 < args.size()) {
          nb_partitions = std::stoi(args[++argidx]);
          continue;
        }


        if (args[argidx] == "-slang") {
          slang = true;
          continue;
        }

     }

     run_script(design, run_from, run_to);
  }

  // -------------------------
  // read_dont_use_file
  // -------------------------
  // Read file 'dont_use_file' made of cells names and add these names in both :
  //       - abc_dont_use_cells
  //       - dont_use_cells
  //
  void read_dont_use_file(string& dont_use_file)
  {
     if (!fs::exists(dont_use_file)) {
       return;
     }

     log("\nReading 'dont_use' cells file '%s'\n", dont_use_file);

     std::ifstream f(dont_use_file);

     for( std::string line; getline(f, line);)
     {
        log("   Dont use cell : %s\n", line);

        abc_dont_use.push_back(line);

        dont_use.push_back(line);
     }

     log("Reading 'dont_use' cells file done !\n");
     log("\n");
     log_flush();
  }

  // -------------------------
  // read_dont_use_files
  // -------------------------
  //
  void read_dont_use_files()
  {
     for (auto d : dont_use_files) {
         read_dont_use_file(d);
     }
  }

  // -------------------------
  // read_project
  // -------------------------
  //
  void read_project()
  {

    if (project == "") {
       return;
    }

    // Project has been set but impossible to access it
    //
    if (!fs::exists(project)) {
       log_error("Cannot access project '%s'\n", project);
       return;
    }

    log("Processing project : '%s'\n", project);

    for (const auto& entry : fs::directory_iterator(project)) {

        if (!entry.is_regular_file()) {
            log_error("'%s' is not a regular file in project '%s'.\n",
                      entry.path(), project);
        }

        fs::path p = entry.path();

        log("   Processing file : '%s'\n", p);

        std::string filename = p.filename().string();
        std::string extension = p.extension().string();

        if ((extension == ".v") ||
           (extension == ".sv") ||
           (extension == ".vh")) {

          verilog_files.push_back(p);

          continue;
        }

        if (extension == ".lib") {
          liberty_files.push_back(p);
          abc_liberty_files.push_back(p);
          continue;
        }

        if (extension == ".sdc") {
          sdc_files.push_back(p);
          continue;
        }

        if (extension == ".def") {
          continue;
        }

        log("\n");
        log("ERROR: Extension '%s' of file '%s' in project '%s' is not supported.\n",
             extension, entry.path(), project);
        log_error("Allowed extensions are : '.v', '.sv', 'vh', '.lib', '.sdc', '.def'.\n");
    }

  }


  // -------------------------
  // read_liberty_files
  // -------------------------
  //
  void read_liberty_files()
  {
     for (auto lib : abc_liberty_files) {
         run("read_liberty -overwrite -setattr liberty_cell -lib " + lib);
         run("read_liberty -overwrite -setattr liberty_cell -unit_delay -wb -ignore_miss_func -ignore_buses " + lib);
     }

     for (auto lib : liberty_files) {
         libertys.push_back(lib);
         run("read_liberty -overwrite -setattr liberty_cell -lib " + lib);
         run("read_liberty -overwrite -setattr liberty_cell -unit_delay -wb -ignore_miss_func -ignore_buses " + lib);
     }

  }

  // -------------------------
  // read_design
  // -------------------------
  //
  void read_design()
  {
     bool file_exists = false;

     if (slang) {

       string cmd = "read_slang --single-unit --threads 112 -D SYNTHESIS ";

       for (auto v : verilog_files) {

          if ((std::filesystem::path(v).extension() != ".v") &&
              (std::filesystem::path(v).extension() != ".sv") &&
              (std::filesystem::path(v).extension() != ".vh"))
          {
            log_error("Verilog file '%s' must have a correct extension (.v, .sv, .vh)\n", v);
          }
          file_exists = true;
          cmd += v + " ";
       }

       if (!file_exists) {
          log_error("Verilog file provided must have correct extensions (.v, .sv, .vh)\n");
       }

       cmd += " --top " + top_module_name + " ";
       cmd += " --allow-use-before-declare --ignore-unknown-modules ";

       // Parse with Slang parser
       //
       run(cmd);

       return;
     }

     // Without 'slang'
     //
     for (auto v : verilog_files) {

        if ((std::filesystem::path(v).extension() != ".v") &&
            (std::filesystem::path(v).extension() != ".vh"))
        {
            log_error("Verilog file '%s' must have a correct extension (.v, .vh)\n", v);
        }

        // Parse with Yosys Parse
        //
        run("read_verilog " + v);
        file_exists = true;
     }

     if (!file_exists) {
        log_error("Verilog file provided must have correct extensions (.v, .vh)\n");
     }
  }

  // -------------------------
  // ends_with_linked
  // -------------------------
  // This error is expected when reading first SDC. So
  // ignore it.
  //
  bool sdc_linked_error(const std::string& str)
  {
    const std::string suffix = "linked.";

    if (str.length() < suffix.length()) {
        return false;
    }

    return str.compare(str.length() - suffix.length(),
                       suffix.length(),
                       suffix) == 0;
  }


  // -------------------------
  // check_sdc
  // -------------------------
  // Try to catch SDC error as soon as possible
  //
  bool check_sdc(const std::string& filename, std::string& error_line)
  {
     std::ifstream file(filename);
     if (!file.is_open()) {
        error_line = "Error: Unable to open file '" + filename + "'.";
        return false;
     }

     std::string line;
     while (std::getline(file, line)) {

        if (line.find("Error:") != std::string::npos) {

            if (sdc_linked_error(line)) {
              continue;
            }
            error_line = line;  // store the full line containing "Error:"
                                // linked.
            return false;
        }
     }

     return true;
  }

  // -------------------------------------------------------
  // MULTI-THREADED PARTITIONNER
  // -------------------------------------------------------

  // ---------------
  // collect_tfi
  // ---------------
  // Recursively traverse the 'bit' fanin (transitive fanin/tfi)
  //
  void collect_tfi(dict<SigBit, tuple<pool<SigBit> *, Cell *>>* sigBits_table,
                   dict<IdString, liberty_cell_info>* liberty_cells_info,
                   pool<IdString>* liberty_buffers,
                   pool<IdString>* liberty_combinationals,
                   int index, SigBit* bit, std::set<Cell*>* cellSet)
  {

    if (sigBits_table->find(*bit) == sigBits_table->end()) { // constant case
      return;
    }

    auto &bitinfo = (*sigBits_table).at(*bit);

    Cell* c = CELL(bitinfo);

    // In case where 'bit' is a primary input (so no cell exists)
    //
    if (c == nullptr) {
       return;
    }

    // Cell already traversed
    //
    if (cellSet->find(c) != cellSet->end()) {
      return;
    }

    cellSet->insert(c);

    if (liberty_cells_info->find(c->type) == liberty_cells_info->end()) {
#if 0
      log("; During 'collect_tfi' , Could not find any liberty cell '%s'\n", c->type);
      log_flush();
#endif
      return;
    }

    // Look if this is a sequential liberty cell. If yes then this is a cut point and
    // return right away.
    //
    if (liberty_cells_info->find(c->type) != liberty_cells_info->end()) {

       liberty_cell_info ca = (*liberty_cells_info)[c->type];

       // A sequential cell is a cut point
       //
       if (ca.is_sequential) {
         return;
       }

       // If the cell is a liberty cell but not sequential, not a comb cell and not a 
       // buffer then we consider it as a cutpoint.
       //
       if ((liberty_combinationals->find(c->type) == liberty_combinationals->end()) &&
           (liberty_buffers->find(c->type) == liberty_buffers->end())) {
          return;
       }

    }

    pool<SigBit> *inputs = INPUTS(bitinfo);

    if (inputs == nullptr) {
      return;
    }

    // Collect recursively in the fanin/inputs 
    //
    for (auto from : *inputs) {

      collect_tfi (sigBits_table, liberty_cells_info, liberty_buffers, liberty_combinationals, 
                   index, &from, cellSet);
    }

  }


  // ----------------------------
  // build_partition
  // ----------------------------
  //
  void build_partition (dict<SigBit, tuple<pool<SigBit> *, Cell *>>* sigBits_table,
                        dict<IdString, liberty_cell_info>* liberty_cells_info,
                        pool<IdString>* liberty_buffers,
                        pool<IdString>* liberty_combinationals,
                        int index, std::vector<pair<SigBit, Cell*>>* endpoint_sublists,
                        std::set<Cell*>* cellSet)
  {
#if 0
     log("; Build Partition %d with size %d\n", index, endpoint_sublists->size());
     log_flush();
#endif

     for (auto bit_cell : *endpoint_sublists) {

         SigBit bit = bit_cell.first;

         collect_tfi (sigBits_table, liberty_cells_info, liberty_buffers, liberty_combinationals,
                      index, &bit, cellSet);
     }
  }

  // ---------------------------------
  // build_design_dictionaries_thread
  // ---------------------------------
  //
  static void build_design_dictionaries_thread(Sinks* actual_users, dict<IdString, Cell*>* string_cells)
  {
    build_design_dictionaries (actual_users, string_cells);
  }

  // -------------------------------
  // get_all_endpoints_pos_thread
  // -------------------------------
  //
  static void get_all_endpoints_pos_thread(Module* top_module, vector<pair<SigBit, Cell*>>* all_endpoints,
		                           Yosys::SigMap* sigmap,
					   dict<IdString, liberty_cell_info>* liberty_cells_info,
					   pool<IdString>* liberty_buffers,
					   pool<IdString>* liberty_combinationals)
  {
     for (auto *w : top_module->wires()) {
         if (w->port_output) {
            for (auto bit : (*sigmap)(w)) {
                all_endpoints->push_back({bit, nullptr});
            }
         }
     }
  }

  // ------------------------------
  // get_all_endpoints_seqs_thread
  // ------------------------------
  //
  static void get_all_endpoints_seqs_thread(Module* top_module, vector<pair<SigBit, Cell*>>* all_endpoints,
		                            Yosys::SigMap* sigmap, 
					    dict<IdString, liberty_cell_info>* liberty_cells_info,
					    pool<IdString>* liberty_buffers,
					    pool<IdString>* liberty_combinationals)
  {

     for (auto cell : top_module->cells()) {

	// Combinational cells are traversable so cannot be an endpoint
	//
        if (liberty_combinationals->find(cell->type) != liberty_combinationals->end()) {
            continue;
        }

	// Buffer cells are traversable so cannot be an endpoint
	//
        if (liberty_buffers->find(cell->type) != liberty_buffers->end()) {
            continue;
        }

	// Non Liberty cells are considered traversable so cannot be an endpoint
	//
        if (liberty_cells_info->find(cell->type) == liberty_cells_info->end()) {
          fprintf(stdout, "WARNING: considering cell '%s' as traversable\n", log_id(cell->type));
          continue;
        }

        liberty_cell_info ca = (*liberty_cells_info)[cell->type];

        if (!ca.is_sequential) {
#if 0
          fprintf(stdout, 
	      "In 'get_all_endpoints_seqs', Considering cell '%s' as cut cell (need to check if this makes sense).\n", 
	      log_id(cell->type));
#endif
        }

	// All inputs of this cell are considered as endpoints
	//
        for (auto &conn : cell->connections()) {

             if (cell->input(conn.first)) {

               SigSpec input_sig = (*sigmap)(conn.second);

               for (auto bit : input_sig.bits()) {
                   all_endpoints->push_back({bit, cell});
               }
             }
        }
    }
  }

  // ----------------------------
  // get_sigBits_table
  // ----------------------------
  //
  static void get_sigBits_table (Module* top_module, 
                                 dict<IdString, liberty_cell_info>* liberty_cells_info,
                                 pool<IdString>* liberty_buffers,
                                 pool<IdString>* liberty_combinationals,
                                 dict<SigBit, tuple<pool<SigBit> *, Cell *>>* sigBits_table,
				 Yosys::SigMap* sigmap)
  {
	  
    for (auto wire : top_module->selected_wires()) {

        SigSpec wire_sig = (*sigmap)(wire);

        for (SigBit bit : wire_sig.bits()) {
           (*sigBits_table)[bit] = tuple<pool<SigBit> *, Cell *>(nullptr, nullptr);
        }
    }

    for (auto cell : top_module->selected_cells()) {

        pool<SigBit> *inputs = new pool<SigBit>;
        vector<SigBit> outputs;

        for (auto &conn : cell->connections()) {

          SigSpec conn_sig = (*sigmap)(conn.second);

          for (auto bit : conn_sig.bits()) {

            if (cell->input(conn.first)) {
              inputs->insert(bit);
              continue;
            }

            if (cell->output(conn.first)) {
              outputs.push_back(bit);
            }
          }
        }

        if (outputs.size() == 0) {
          fprintf(stdout, "WARNING: It seems cell '%s' (%s) has no output (dangling cell).\n",
                     log_id(cell->name), log_id(cell->type));
          continue;
        }

        for (auto out : outputs) {

           auto &bitinfo = (*sigBits_table).at(out);

           INPUTS(bitinfo) = inputs;
           CELL(bitinfo) = cell;
        }
     }

  }

  // ----------------------------
  // report_run_time
  // ----------------------------
  //
  void report_run_time(const std::chrono::high_resolution_clock::time_point& start_time,
                       const char* message)
  {
      auto end_time = std::chrono::high_resolution_clock::now();

      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);

      double total_time_sec = elapsed.count() * 1e-9;

      log(message, total_time_sec);
      log_flush();
  }

  // ----------------------------
  // partition_design
  // ----------------------------
  //
  void partition_design (int nb_partitions, vector<Design*>& partitions)
  {
     auto startTime = std::chrono::high_resolution_clock::now();

     log(" Performing Sta Partition ...\n");

     log_assert(nb_partitions > 0);

     Design* design = yosys_get_design();
     Module* top_module = design->top_module();

     Yosys::SigMap sigmap(top_module);

     report_run_time(startTime, "\n; ['SigMap' Run Time = %.3f sec.]\n\n");


     dict<IdString, liberty_cell_info> liberty_cells_info = g_liberty_cells_info;
     pool<IdString> liberty_buffers = g_liberty_buffers;
     pool<IdString> liberty_combinationals = g_liberty_combinationals;

     // --------------------------------------
     // 1. Collect all primary outputs sigBits
     //
     // First create a pool of "actual users" (Sinks) :
     //
     //    for a SigSpec store the vector of triplets <Cell, portName, bus_index>
     //    that this SigSpec is driving.
     //
     Sinks actual_users;
     dict<IdString, Cell*> string_cells;


     std::thread thread1(build_design_dictionaries_thread, &actual_users, &string_cells);

     // --------------------------------------
     // 2. Collect all primary outputs sigBits
     //
     vector<pair<SigBit, Cell*>> all_endpoints_pos;

     std::thread thread2(get_all_endpoints_pos_thread, top_module, &all_endpoints_pos, &sigmap,
		         &liberty_cells_info, &liberty_buffers, &liberty_combinationals);

     // -------------------------------------------
     // 3. Collect pin inputs sigBits of seq cells 
     //
     vector<pair<SigBit, Cell*>> all_endpoints_seqs;

     std::thread thread3(get_all_endpoints_seqs_thread, top_module, &all_endpoints_seqs, &sigmap,
		         &liberty_cells_info, &liberty_buffers, &liberty_combinationals);

     // -------------------------------------------
     // 4. Build the SigBit table giving relationship beetween Cell inputs and cell
     // output. This will be used by the TFI based algo to do the backward traversal.
     //
     dict<SigBit, tuple<pool<SigBit> *, Cell *>> sigBits_table;

     std::thread thread4(get_sigBits_table, top_module, &liberty_cells_info, &liberty_buffers, 
                         &liberty_combinationals, &sigBits_table, &sigmap);


     // Wait for all threads to complete
     //
     thread1.join();
     thread2.join();
     thread3.join();
     thread4.join();

     log("; Size of all_ednpoints_pos = %d\n", all_endpoints_pos.size());
     log_flush();
     log("; Size of all_ednpoints_seqs = %d\n", all_endpoints_seqs.size());
     log_flush();

     report_run_time(startTime, "; ['get_all_endpoints' Run Time = %.3f sec.]\n");

     all_endpoints_pos.insert(all_endpoints_pos.end(), all_endpoints_seqs.begin(), all_endpoints_seqs.end());

     vector<pair<SigBit, Cell*>> all_endpoints = all_endpoints_pos;

     log("; Found in total '%zu' endpoints.\n", all_endpoints.size());
     log(";\n");
     log_flush();

     report_run_time(startTime, "\n; ['Building all_endpoints vectors' took %.3f sec.]\n\n");

     // ------------------------------------------------------------
     // 5. Cut the list of endpoints into 'nb_partitions' sublists
     //
     size_t num_total_endpoints = all_endpoints.size();

     std::vector<std::vector<pair<SigBit, Cell*>>> endpoint_sublists;

     for (int i = 0; i < nb_partitions+1; ++i) {
         endpoint_sublists.emplace_back(); // Add empty sublist
     }
     
     // -----------------------------------------------
     // 6. Distribute endpoints as evenly as possible
     //
     int slice_width = (num_total_endpoints / nb_partitions) + 1;

     if (slice_width == 0) {
       slice_width++;
     }

     // Put together contiguous endpoints, I noticed way smaller partitions
     // because contiguous endpoints may involve more contiguous logic which is
     // better shared withinh a partition.
     //
     for (size_t i = 0; i < num_total_endpoints; ++i) {

         //fprintf(stdout, "endpoint_sublists index = %ld\n", i / slice_width);
         endpoint_sublists[i / slice_width].push_back(all_endpoints[i]);

     }

     std::vector<std::thread> workers(nb_partitions);

     // One set per thread
     std::vector<std::set<Cell*>> thread_sets(nb_partitions);

     // ---- worker lambda ----
     auto worker = [&](int i)
     {
        this->build_partition(
            &sigBits_table,
            &liberty_cells_info,
            &liberty_buffers,
            &liberty_combinationals,
            i,
            &endpoint_sublists[i],
            &thread_sets[i]
        );
     };

     // ---- launch threads ----
     for (int i = 0; i < nb_partitions; ++i) {
        workers[i] = std::thread(worker, i);
     }

     // ---- join threads ----
     for (int i = 0; i < nb_partitions; ++i) {
        workers[i].join();
     }

#if 0
     std::sort(thread_sets.begin(), thread_sets.end(),
               [](const std::set<Cell*>& a,
                  const std::set<Cell*>& b)
               {
                 return a.size() < b.size();
               }
     );
#endif

     // ---- logging AFTER all threads finish ----
     int nb_endp = 0;
     int nb_cells = 0;

     for (int i = 0; i < nb_partitions; ++i) {

        log(";   Partition %d : '%zu' endpoints, '%zu' cells\n",
            i,
            endpoint_sublists[i].size(),
            thread_sets[i].size());

        log_flush();

	nb_endp += endpoint_sublists[i].size();
	nb_cells += thread_sets[i].size();
     }

     log("\n");
     log("Sum of end points over all partitions  = %ld\n", nb_endp);
     log("Sum of cells over all partitions       = %ld\n", nb_cells);

     if (all_endpoints.size() != nb_endp) {
       log_error("Mismatch between total endpoints and sum of all endpoints partitions !\n");
     }

     log("\n");
     log("Partitions built !\n");
     log_flush();

     report_run_time(startTime, " ['Build all partitions' Run Time = %.3f sec.]\n");
     


     // ---------------------------------------------
     //
     report_run_time(startTime, " [Design Partitioner Run Time = %.3f sec.]\n\n");

  }

  // ----------------------------
  // script
  // ----------------------------
  //
  void script() override {

    // Register Ctrl-C handler
    //
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGSEGV, handle_sigsegv);
    std::signal(SIGBUS, handle_sigsegv);
    std::signal(SIGILL, handle_sigsegv);

    try {

       log(" STA PARTITION pass ...\n");
       log("\n");

       log(" Top Module = %s\n", top_module_name);
       log("\n");

       log(" Reading Project ...\n");
       read_project();
       log("\n");

       log(" Reading Liberty Files ...\n");
       read_liberty_files();
       log("\n");

       log(" Analyzing Liberty Files ...\n");
       analyze_all_liberty_files (libertys);
       log("\n");

       log(" Parsing RTL ...\n");
       read_design();
       log("\n");

       log(" Reading Dont_Use Cells ...\n");
       read_dont_use_files();
       log("\n");

       log(" Performing Sta Partition ...\n");

       vector<Design*> partitions;

       partition_design(nb_partitions, partitions);
       log("\n");
    }

    catch (...) {

      // we prefer to write in stdout than stderr for central catch.
      //
      fprintf(stdout, "\n");
      fprintf(stdout, "ERROR: Abort due to internal error in 'sta_partition'.\n");

    }
  }

} StaPartition;

PRIVATE_NAMESPACE_END
