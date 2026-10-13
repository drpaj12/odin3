# generated: elab-check --write
attribute \top 1
module \i4_widths
  wire width 2 input 1 \narrow
  wire width 8 input 2 \wide
  wire width 12 output 3 \y1
  wire width 4 output 4 \y2
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  cell \i4_leaf \u1
    connect \a { 2'00 \narrow }
    connect \y \y1 [7:0]
  end
  cell \i4_leaf \u2
    connect \a \wide [3:0]
    connect \y { $n31 $n30 $n29 $n28 \y2 }
  end
  connect \y1 [11:8] { \y1 [7] \y1 [7] \y1 [7] \y1 [7] }
end
module \i4_leaf
  wire width 4 input 1 signed \a
  wire width 8 output 2 signed \y
  connect \y { \a [3] \a [3] \a [3] \a [3] \a }
end
