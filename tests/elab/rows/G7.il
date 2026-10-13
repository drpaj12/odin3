# generated: elab-check --write
attribute \top 1
module \g7_scope_names
  wire width 4 input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  wire output 4 \y3
  wire \g[0].t
  wire \g[1].t
  wire \g[2].t
  wire \g[3].t
  wire \genblk2.w
  connect \y1 \a [0]
  connect \y2 \a [3]
  connect \y3 \a [0]
  connect \g[0].t \a [0]
  connect \g[1].t \a [1]
  connect \g[2].t \a [2]
  connect \g[3].t \a [3]
  connect \genblk2.w \a [0]
end
