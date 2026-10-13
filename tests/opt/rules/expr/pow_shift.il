attribute \top 1
module \t
  wire width 4 input 1 \b
  wire width 8 output 2 \y
  wire width 8 \k
  cell $shl $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A 1'1
    connect \B \b
    connect \Y \y
  end
  connect \k 8'00000010
end
