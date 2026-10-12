module \top
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y
  cell $mux \m
    parameter \WIDTH 1
    connect \A \a
    connect \B \b
    connect \S 1'0
    connect \Y \y
  end
end
