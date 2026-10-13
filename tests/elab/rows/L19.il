# generated: elab-check --write
attribute \top 1
module \l19_top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  cell \l19_none \u1
    connect \a \a
    connect \y \y1
  end
  cell \l19_wire \u2
    connect \a \a
    connect \y \y2
  end
end
module \l19_none
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \l19_wire
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
