module \m
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y
  wire output 4 \z
  wire \t1
  wire \t2
  cell $and \a1
    connect \A \a
    connect \B \b
    connect \Y \t1
  end
  cell $and \a2
    connect \A \a
    connect \B \b
    connect \Y \t2
  end
  cell $not \n1
    connect \A \t1
    connect \Y \y
  end
  cell $not \n2
    connect \A \t2
    connect \Y \z
  end
end
