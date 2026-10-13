# generated: elab-check --write
attribute \top 1
module \m3_top
  wire input 1 \a
  wire output 2 \q1
  wire output 3 \q2
  wire width 4 output 4 \r1
  wire width 32 output 5 \n1
  wire width 32 output 6 \n2
  cell \m3_ansi \u1
    connect \a \a
    connect \q \q1
    connect \r \r1
    connect \n \n1
  end
  cell \m3_nonansi \u2
    connect \a \a
    connect \q \q2
    connect \n \n2
  end
end
module \m3_ansi
  wire input 1 \a
  wire output 2 \q
  wire width 4 output 3 \r
  wire width 32 output 4 signed \n
end
module \m3_nonansi
  wire input 1 \a
  wire output 2 \q
  wire width 32 output 3 signed \n
end
