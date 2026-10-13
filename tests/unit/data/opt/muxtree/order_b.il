module \top
  wire input 1 \s0
  wire input 2 \s1
  wire input 3 \s2
  wire input 4 \d0
  wire input 5 \d1
  wire input 6 \d2
  wire input 7 \d3
  wire \y0
  wire \y1
  wire \y2
  wire \y3
  wire \y4
  wire \y5
  wire \y6
  wire \y7
  wire \y8
  wire \y9
  wire output 8 \y10
  wire \y11
  wire output 9 \y12
  wire output 10 \y13
  wire \y14
  wire \y15
  wire output 11 \y16
  cell $mux \m0
    parameter \WIDTH 1
    connect \A 1'0
    connect \B { 1'0 }
    connect \S { \s1 }
    connect \Y \y0
  end
  cell $mux \m1
    parameter \WIDTH 1
    connect \A 1'1
    connect \B { 1'1 }
    connect \S { \s1 }
    connect \Y \y1
  end
  cell $mux \m2
    parameter \WIDTH 1
    connect \A \y0
    connect \B { \y0 }
    connect \S { \s1 }
    connect \Y \y2
  end
  cell $mux \m3
    parameter \WIDTH 1
    connect \A \y1
    connect \B { \y0 }
    connect \S { \s1 }
    connect \Y \y3
  end
  cell $mux \m4
    parameter \WIDTH 1
    connect \A \y1
    connect \B { \d0 }
    connect \S { \s1 }
    connect \Y \y4
  end
  cell $mux \m5
    parameter \WIDTH 1
    connect \A \y4
    connect \B { \y4 }
    connect \S { 1'0 }
    connect \Y \y5
  end
  cell $pmux \m6
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \y4
    connect \B { \d3 \y4 \y4 }
    connect \S { \s0 \s1 1'1 }
    connect \Y \y6
  end
  cell $mux \m7
    parameter \WIDTH 1
    connect \A \y6
    connect \B { \d0 }
    connect \S { \s2 }
    connect \Y \y7
  end
  cell $mux \m8
    parameter \WIDTH 1
    connect \A \y5
    connect \B { \y6 }
    connect \S { \s2 }
    connect \Y \y8
  end
  cell $mux \m9
    parameter \WIDTH 1
    connect \A \y5
    connect \B { \y6 }
    connect \S { \s2 }
    connect \Y \y9
  end
  cell $mux \m10
    parameter \WIDTH 1
    connect \A \y9
    connect \B { \y9 }
    connect \S { \s0 }
    connect \Y \y10
  end
  cell $pmux \m11
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \y9
    connect \B { \y9 \y8 }
    connect \S { \s1 \s0 }
    connect \Y \y11
  end
  cell $pmux \m12
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \y8
    connect \B { \d3 \y8 }
    connect \S { \s2 \s0 }
    connect \Y \y12
  end
  cell $pmux \m13
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \y0
    connect \B { \s0 \y12 \y11 }
    connect \S { \s1 \s0 \s2 }
    connect \Y \y13
  end
  cell $pmux \m14
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \y11
    connect \B { 1'0 1'1 }
    connect \S { \s1 \s0 }
    connect \Y \y14
  end
  cell $mux \m15
    parameter \WIDTH 1
    connect \A \y10
    connect \B { \y10 }
    connect \S { \s1 }
    connect \Y \y15
  end
  cell $mux \m16
    parameter \WIDTH 1
    connect \A \y13
    connect \B { \y9 }
    connect \S { \s1 }
    connect \Y \y16
  end
end
