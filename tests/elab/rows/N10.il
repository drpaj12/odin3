# generated: elab-check --write
attribute \top 1
module \n10_scalar_array
  wire width 2 input 1 \i
  wire output 2 \y
  wire $n4
  cell $mem $c5
    parameter \MEMID "\\a"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 2
    parameter \WIDTH 1
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \i
    connect \RD_DATA $n4
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  connect \y $n4
end
