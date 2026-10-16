attribute \top 1
module \port_a
  wire width 4 input 1 \a
  wire width 8 input 2 \b
  wire width 8 output 3 \y
  cell $add $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
