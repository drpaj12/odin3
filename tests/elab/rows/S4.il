# generated: elab-check --write
attribute \top 1
module \s4_readmem
  wire width 4 input 1 \addr
  wire width 8 output 2 \y
  wire width 8 output 3 \z
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  cell $mem $c6
    parameter \MEMID "\\rom_h"
    parameter \SIZE 16
    parameter \OFFSET 0
    parameter \ABITS 4
    parameter \WIDTH 8
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 128'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1111000011011110101111001001101001111000010101100011010000010010
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \addr
    connect \RD_DATA { $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  cell $mem $c7
    parameter \MEMID "\\rom_b"
    parameter \SIZE 16
    parameter \OFFSET 0
    parameter \ABITS 4
    parameter \WIDTH 8
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 128'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx000001000000001000000001
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \addr
    connect \RD_DATA { $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  connect \y { $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 }
  connect \z { $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 }
end
