# generated: elab-check --write
attribute \top 1
module \a1_assign
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire width 8 output 4 \w
  wire output 5 \c
  wire width 4 output 6 \s
  wire output 7 \p
  wire output 8 \q
  cell $add $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { \c \s }
  end
  connect \y \a
  connect \w { \b \a }
  connect \p \a [1]
  connect \q \b [1]
end
