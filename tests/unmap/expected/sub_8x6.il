attribute \top 1
module \sub_8x6
  wire width 8 input 1 \a
  wire width 6 input 2 \b
  wire width 9 output 3 \y
  attribute \unmap "chain"
  attribute \unmap_pieces 16
  cell $sub $c24
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 6
    parameter \Y_WIDTH 9
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
