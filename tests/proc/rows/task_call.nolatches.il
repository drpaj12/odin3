attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire width 4 output 4 \z
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y
  end
  cell $sub $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \z
  end
end
