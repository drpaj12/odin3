# generated: elab-check --write
attribute \top 1
module \m8_top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  cell \m8_macro \u1
    connect \a \a
    connect \y \y1
  end
  cell \m8_macro_nonansi \u2
    connect \a \a
    connect \y \y2
  end
end
module \m8_macro
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \m8_macro_nonansi
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
