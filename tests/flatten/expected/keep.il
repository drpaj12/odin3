module \top
  wire input 1 \x
  wire input 2 \z
  wire output 3 \o
  wire output 4 \p
  wire output 5 \q
  wire \e.a
  wire \e.b
  wire \e.y
  cell \kmod \k
    connect \a \x
    connect \b \z
    connect \y \o
  end
  attribute \keep_hierarchy 1
  cell \leaf \ki
    connect \a \x
    connect \b \z
    connect \y \p
  end
  cell $or $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x
    connect \B \z
    connect \Y \q
  end
  connect \e.a \x
  connect \e.b \z
  connect \e.y \q
end
