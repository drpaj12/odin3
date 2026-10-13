# generated: elab-check --write
attribute \top 1
module \l22_top
  wire input 1 \a
  wire width 2 input 2 \b
  wire output 3 \y1
  wire output 4 \y2
  wire output 5 \y3
  wire output 6 \y4
  wire \unused
  cell \l22_1995 \u1
    connect \a \a
    connect \y \y1
  end
  cell \l22_2001 \u2
    connect \a \b
    connect \uwire \unused
  end
  cell \l22_noconfig \u3
    connect \a \a
    connect \config \y3
  end
  cell \l22_2005 \u4
    connect \a \a
    connect \y \y4
  end
  connect \y2 \unused
end
module \l22_1995
  wire input 1 \a
  wire output 2 \y
  wire \generate
  connect \y \a
  connect \generate \a
end
module \l22_2001
  wire width 2 input 1 signed \a
  wire output 2 \uwire
  connect \uwire \a [0]
end
module \l22_noconfig
  wire input 1 \a
  wire output 2 \config
  connect \config \a
end
module \l22_2005
  wire input 1 \a
  wire output 2 \y
  wire \w
  connect \y \a
  connect \w \a
end
