attribute \top 1
module \spram_instance
  wire width 8 input 1 \address
  wire width 16 input 2 \value_in
  wire input 3 \we
  wire input 4 \clock
  wire width 16 output 5 \value_out
  attribute \unmap "ram.width"
  attribute \unmap_pieces 16
  attribute \unmap_origin 8
  cell $mem \inst1
    parameter \MEMID "\\inst1"
    parameter \SIZE 256
    parameter \OFFSET 0
    parameter \ABITS 8
    parameter \WIDTH 16
    parameter \RD_PORTS 1
    parameter \WR_PORTS 1
    parameter \RD_CLK_ENABLE 1'1
    parameter \RD_CLK_POLARITY 1'1
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    parameter \INIT 0'
    connect \RD_CLK \clock
    connect \RD_EN 1'1
    connect \RD_ADDR \address
    connect \RD_DATA \value_out
    connect \WR_CLK \clock
    connect \WR_EN { \we \we \we \we \we \we \we \we \we \we \we \we \we \we \we \we }
    connect \WR_ADDR \address
    connect \WR_DATA \value_in
  end
end
