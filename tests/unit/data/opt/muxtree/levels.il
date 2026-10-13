module \top
  wire input 1 \s0
  wire input 2 \t1
  wire input 3 \t2
  wire input 4 \p
  wire input 5 \a1
  wire input 6 \a2
  wire input 7 \a3
  wire input 8 \a4
  wire input 9 \x4
  wire input 10 \z1
  wire input 11 \z2
  wire input 12 \z3
  wire input 13 \z4
  wire output 14 \yp
  wire output 15 \yd1
  wire output 16 \yd2
  wire output 17 \yd3
  wire output 18 \yd4
  wire \r1
  wire \r2
  wire \r3
  wire \r4
  cell $mux \P
    parameter \WIDTH 1
    connect \A \p
    connect \B \r1
    connect \S \s0
    connect \Y \yp
  end
  cell $mux \R1
    parameter \WIDTH 1
    connect \A \a1
    connect \B \r2
    connect \S \t1
    connect \Y \r1
  end
  cell $mux \R2
    parameter \WIDTH 1
    connect \A \a2
    connect \B \r3
    connect \S \t2
    connect \Y \r2
  end
  cell $mux \R3
    parameter \WIDTH 1
    connect \A \a3
    connect \B \r4
    connect \S \s0
    connect \Y \r3
  end
  cell $mux \R4
    parameter \WIDTH 1
    connect \A \a4
    connect \B \x4
    connect \S \s0
    connect \Y \r4
  end
  cell $mux \D1
    parameter \WIDTH 1
    connect \A \z1
    connect \B \r1
    connect \S \s0
    connect \Y \yd1
  end
  cell $mux \D2
    parameter \WIDTH 1
    connect \A \z2
    connect \B \r2
    connect \S \s0
    connect \Y \yd2
  end
  cell $mux \D3
    parameter \WIDTH 1
    connect \A \z3
    connect \B \r3
    connect \S \s0
    connect \Y \yd3
  end
  cell $mux \D4
    parameter \WIDTH 1
    connect \A \z4
    connect \B \r4
    connect \S \s0
    connect \Y \yd4
  end
end
