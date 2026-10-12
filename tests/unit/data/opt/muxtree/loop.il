module \top
  wire input 1 \s1
  wire input 2 \s2
  wire input 3 \a
  wire input 4 \b
  wire output 5 \y1
  wire output 6 \y2
  cell $mux \m1
    parameter \WIDTH 1
    connect \A \a
    connect \B \y2
    connect \S \s1
    connect \Y \y1
  end
  cell $mux \m2
    parameter \WIDTH 1
    connect \A \b
    connect \B \y1
    connect \S \s2
    connect \Y \y2
  end
end
