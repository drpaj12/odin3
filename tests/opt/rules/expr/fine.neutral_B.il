attribute \top 1
module \t
  wire input 1 \a
  wire width 2 input 2 \b
  wire output 3 \y
  cell $logic_and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
