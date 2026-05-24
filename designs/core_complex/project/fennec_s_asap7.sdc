# clk_cpu clock
#
set clk_cpu_period 1
set clk_cpu_io_pct 0.2

set clk_cpu_port [get_ports clk_cpu]

create_clock -name clk_cpu -period $clk_cpu_period $clk_cpu_port

set non_core_clock_inputs [lsearch -inline -all -not -exact [all_inputs] $clk_cpu_port]
set_input_delay [expr {$clk_cpu_period * $clk_cpu_io_pct}] -clock clk_cpu $non_core_clock_inputs
set_output_delay [expr {$clk_cpu_period * $clk_cpu_io_pct}] -clock clk_cpu [all_outputs]


# clk_sys clock
#
set clk_sys_period 1
set clk_sys_io_pct 0.2

set clk_sys_port [get_ports clk_sys]

create_clock -name clk_sys -period $clk_sys_period $clk_sys_port

set non_uncore_clock_inputs [lsearch -inline -all -not -exact [all_inputs] $clk_sys_port]
set_input_delay [expr {$clk_sys_period * $clk_sys_io_pct}] -clock clk_sys $non_uncore_clock_inputs
set_output_delay [expr {$clk_sys_period * $clk_sys_io_pct}] -clock clk_sys [all_outputs]



# jtag_tck clock
#
set jtag_tck_period 1
set jtag_tck_io_pct 0.2

set jtag_tck_port [get_ports jtag_tck]

create_clock -name jtag_tck -period $jtag_tck_period $jtag_tck_port

set non_jtag_TCK_clock_inputs [lsearch -inline -all -not -exact [all_inputs] $jtag_tck_port]
set_input_delay [expr {$jtag_tck_period * $jtag_tck_io_pct}] -clock jtag_tck $non_jtag_TCK_clock_inputs
set_output_delay [expr {$jtag_tck_period * $jtag_tck_io_pct}] -clock jtag_tck [all_outputs]



set_driving_cell -lib_cell BUFx2_ASAP7_75t_R [all_inputs]

