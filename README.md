# UWB_CIR

#### Extract CIR data for classification of LoS/NLoS situation. 

## Requirements

- #### Some terminal
- #### sw4stm32 IDE

## How to use

* Prerequisite: Install the PCL298336 daughterboard to the Nucleo, and open "SB122" jumper and short "SB121" jumper of the Nucleo (the initialization fails otherwise).
#### 1. edit `main.c` and `example_selection.h` to target the correct example project (ex_01a_simple_tx for the Tx node, ex_02c_rx_diagnostics for the Rx node). 
#### 2. compile and install the project.
#### 3. Extract the data over the secondary UART (you need to connect both the upper and lower microUSB connectors to the host PC). 
