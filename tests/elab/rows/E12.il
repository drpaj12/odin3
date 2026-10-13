# generated: elab-check --write
attribute \top 1
module \e12_concat
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 8 output 3 \y1
  wire width 9 output 4 \y2
  wire width 12 output 5 \y3
  wire width 4 output 6 \y4
  wire output 7 \c
  wire width 4 output 8 \s
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { \c \s }
  end
  connect \y1 { \a \b }
  connect \y2 { \a 2'01 \b [2:0] }
  connect \y3 { \a \b \a }
  connect \y4 \a
end
