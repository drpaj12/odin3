attribute \top 1
module \mul_8x7
  wire width 8 input 1 \a
  wire width 7 input 2 \b
  wire width 15 output 3 \y
  attribute \unmap "mul"
  attribute \unmap_pieces 1
  attribute \unmap_w 36
  cell $mul $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 7
    parameter \Y_WIDTH 15
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
