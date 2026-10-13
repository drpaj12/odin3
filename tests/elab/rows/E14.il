# generated: elab-check --write
attribute \top 1
module \e14_const_select
  wire width 8 input 1 \a
  wire output 2 \y1
  wire width 4 output 3 \y2
  wire output 4 \y3
  wire width 8 output 5 \y4
  connect \y1 \a [0]
  connect \y2 \a [7:4]
  connect \y3 \a [3]
  connect \y4 [2:0] { \a [3] \a [1:0] }
end
