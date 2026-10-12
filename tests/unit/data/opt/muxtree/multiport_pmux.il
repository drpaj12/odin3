module \top
  wire input 1 \s0
  wire input 2 \s1
  wire input 3 \t
  wire input 4 \a
  wire input 5 \b
  wire output 6 \y
  wire \c
  cell $mux \C
    parameter \WIDTH 1
    connect \A \s0
    connect \B \b
    connect \S \t
    connect \Y \c
  end
  cell $pmux \M
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \c \c }
    connect \S { \s1 \s0 }
    connect \Y \y
  end
end
