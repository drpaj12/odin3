# generated: elab-check --write
attribute \top 1
module \i7_black_box
  wire inout 1 \p1
  wire inout 2 \q1
  wire inout 3 \r
  wire inout 4 \p2
  wire inout 5 \q2
  wire inout 6 \s
  cell \black_box_cell \u1
    connect \a \p1
    connect \b \q1
    connect \y \r
  end
  cell \black_box_cell \u2
    connect \a \p2
    connect \b \q2
    connect \y \s
  end
end
attribute \blackbox 1
module \black_box_cell
  wire inout 1 \a
  wire inout 2 \b
  wire inout 3 \y
end
