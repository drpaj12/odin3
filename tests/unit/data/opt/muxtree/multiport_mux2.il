module \top
  wire input 1 \s
  wire input 2 \t
  wire width 2 input 3 \b
  wire output 4 \y
  wire width 2 \c
  cell $mux \C
    parameter \WIDTH 2
    connect \A { \s \s }
    connect \B \b
    connect \S \t
    connect \Y \c
  end
  cell $mux \M
    parameter \WIDTH 1
    connect \A \c [1]
    connect \B \c [0]
    connect \S \s
    connect \Y \y
  end
end
