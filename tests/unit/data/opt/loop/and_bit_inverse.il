module \m
  wire input 1 \a
  wire output 2 \y
  wire \n
  cell $not \inv
    connect \A \a
    connect \Y \n
  end
  cell $and \c
    connect \A \a
    connect \B \n
    connect \Y \y
  end
end
