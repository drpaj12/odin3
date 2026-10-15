module \top
  wire input 1 \x
  wire output 2 \o
  wire output 3 \p
  wire \m.a
  wire \m.y
  cell \bbox \t
    connect \a \x
    connect \y \p
  end
  cell \bbox \m.b
    connect \a \x
    connect \y \o
  end
  connect \m.a \x
  connect \m.y \o
end
