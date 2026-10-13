# generated: elab-check --write
attribute \top 1
module \l20_top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  cell \l20_a \u1
    connect \a \a
    connect \y \y1
  end
  cell \l20_b \u2
    connect \a \a
    connect \y \y2
  end
end
module \l20_a
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \l20_b
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
