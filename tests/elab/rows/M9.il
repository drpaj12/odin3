# generated: elab-check --write
attribute \top 1
module \m9_top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  wire output 4 \y3
  cell \m9_first \u1
    connect \a \a
    connect \y \y1
  end
  cell \m9_second \u2
    connect \a \a
    connect \y \y2
  end
  cell \m9_third \u3
    connect \a \a
    connect \y \y3
  end
end
module \m9_first
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \m9_second
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \m9_third
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
