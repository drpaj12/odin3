attribute \top 1
module \mul_40x40
  wire width 40 input 1 \a
  wire width 40 input 2 \b
  wire width 80 output 3 \p
  attribute \unmap "mul.p2"
  attribute \unmap_pieces 6
  attribute \unmap_w 36
  cell $mul $c111
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 40
    parameter \B_WIDTH 40
    parameter \Y_WIDTH 80
    connect \A \a
    connect \B \b
    connect \Y \p
  end
end
