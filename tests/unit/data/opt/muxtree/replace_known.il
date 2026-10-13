module \top
  wire input 1 \s
  wire input 2 \u
  wire input 3 \v
  wire input 4 \a
  wire input 5 \a2
  wire input 6 \c
  wire output 7 \y
  wire \i1
  wire \i2
  cell $mux \in1
    parameter \WIDTH 1
    connect \A \s
    connect \B \a
    connect \S \u
    connect \Y \i1
  end
  cell $mux \in2
    parameter \WIDTH 1
    connect \A \s
    connect \B \a2
    connect \S \v
    connect \Y \i2
  end
  cell $mux \outer
    parameter \WIDTH 1
    connect \A \i2
    connect \B \i1
    connect \S \s
    connect \Y \y
  end
end
