attribute \top 1
module \t
  wire width 2 input 1 \a
  wire width 3 input 2 \b
  wire output 3 \y
  wire width 8 \t
  cell $shiftx $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
  connect \t { 6'xxxxxx \a }
end
