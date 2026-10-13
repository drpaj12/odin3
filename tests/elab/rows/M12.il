# generated: elab-check --write
attribute \top 1
module \m12_top
  wire input 1 \a
  wire width 32 input 2 \n
  wire output 3 \y1
  wire output 4 \y2
  cell \m12_nonansi \u1
    connect \a \a
    connect \n \n
    connect \y \y1
  end
  cell \m12_ansi \u2
    connect \a \a
    connect \n \n
    connect \y \y2
  end
end
module \m12_nonansi
  wire input 1 \a
  wire width 32 input 2 signed \n
  wire output 3 \y
  connect \y \a
end
module \m12_ansi
  wire input 1 \a
  wire width 32 input 2 signed \n
  wire output 3 \y
  connect \y \a
end
