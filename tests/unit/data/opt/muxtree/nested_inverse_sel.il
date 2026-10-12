module \top
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire input 4 \c
  wire output 5 \y
  wire \t
  cell $mux \inner
    parameter \WIDTH 1
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \t
  end
  cell $mux \outer
    parameter \WIDTH 1
    connect \A \t
    connect \B \c
    connect \S \s
    connect \Y \y
  end
end
