attribute \top 1
module \t
  wire input 1 \c
  wire width 2 input 2 \wa
  wire width 2 input 3 \ra
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  cell $mem $c9
    parameter \MEMID "\\m"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 2
    parameter \WIDTH 4
    parameter \RD_PORTS 1
    parameter \WR_PORTS 1
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \ra
    connect \RD_DATA { $n17 $n16 $n15 $n14 }
    connect \WR_CLK \c
    connect \WR_EN 4'1111
    connect \WR_ADDR \wa
    connect \WR_DATA \d
  end
  cell $dff $c10
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \c
    connect \D { $n17 $n16 $n15 $n14 }
    connect \Q \q
  end
end
